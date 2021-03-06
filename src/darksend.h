// Copyright (c) 2014-2016 The Vivoinnovaonexgobyte Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DARKSEND_H
#define DARKSEND_H

#include "main.h"
#include "sync.h"
#include "activemasternode.h"
#include "masternodeman.h"
#include "darksend-relay.h"

class CTxIn;
class CDarksendPool;
class CDarkSendSigner;
class CMasterNodeVote;
class CBitcoinAddress;
class CDarksendQueue;
class CDarksendBroadcastTx;
class CActiveMasternode;

// pool states for mixing
#define POOL_STATUS_UNKNOWN                    0 // waiting for update
#define POOL_STATUS_IDLE                       1 // waiting for update
#define POOL_STATUS_QUEUE                      2 // waiting in a queue
#define POOL_STATUS_ACCEPTING_ENTRIES          3 // accepting entries
#define POOL_STATUS_FINALIZE_TRANSACTION       4 // master node will broadcast what it accepted
#define POOL_STATUS_SIGNING                    5 // check inputs/outputs, sign final tx
#define POOL_STATUS_TRANSMISSION               6 // transmit transaction
#define POOL_STATUS_ERROR                      7 // error
#define POOL_STATUS_SUCCESS                    8 // success

// status update message constants
#define MASTERNODE_ACCEPTED                    1
#define MASTERNODE_REJECTED                    0
#define MASTERNODE_RESET                       -1

#define DARKSEND_QUEUE_TIMEOUT                 30
#define DARKSEND_SIGNING_TIMEOUT               15

// used for anonymous relaying of inputs/outputs/sigs
#define DARKSEND_RELAY_IN                 1
#define DARKSEND_RELAY_OUT                2
#define DARKSEND_RELAY_SIG                3

static const CAmount DARKSEND_COLLATERAL = (0.01*COIN);
static const CAmount DARKSEND_POOL_MAX = (999.99*COIN);
static const CAmount DENOMS_COUNT_MAX = 100;
// Warn user if mixing in gui or try to create backup if mixing in daemon mode
// when we have only this many keys left
static const int PS_KEYS_THRESHOLD_WARNING = 100;
// Stop mixing completely, it's too dangerous to continue when we have only this many keys left
static const int PS_KEYS_THRESHOLD_STOP = 50;

extern CDarksendPool darkSendPool;
extern CDarkSendSigner darkSendSigner;
extern std::vector<CDarksendQueue> vecDarksendQueue;
extern std::string strMasterNodePrivKey;
extern map<uint256, CDarksendBroadcastTx> mapDarksendBroadcastTxes;
extern CActiveMasternode activeMasternode;

/** Holds an Darksend input
 */
class CTxDSIn : public CTxIn
{
public:
    bool fHasSig; // flag to indicate if signed
    int nSentTimes; //times we've sent this anonymously

    CTxDSIn(const CTxIn& in)
    {
        prevout = in.prevout;
        scriptSig = in.scriptSig;
        prevPubKey = in.prevPubKey;
        nSequence = in.nSequence;
        nSentTimes = 0;
        fHasSig = false;
    }
};

/** Holds an Darksend output
 */
class CTxDSOut : public CTxOut
{
public:
    int nSentTimes; //times we've sent this anonymously

    CTxDSOut(const CTxOut& out)
    {
        nValue = out.nValue;
        nRounds = out.nRounds;
        scriptPubKey = out.scriptPubKey;
        nSentTimes = 0;
    }
};

// A clients transaction in the darksend pool
class CDarkSendEntry
{
public:
    bool isSet;
    std::vector<CTxDSIn> sev;
    std::vector<CTxDSOut> vout;
    CAmount amount;
    CTransaction collateral;
    CTransaction txSupporting;
    int64_t addedTime; // time in UTC milliseconds

    CDarkSendEntry()
    {
        isSet = false;
        collateral = CTransaction();
        amount = 0;
    }

    /// Add entries to use for Darksend
    bool Add(const std::vector<CTxIn> vinIn, CAmount amountIn, const CTransaction collateralIn, const std::vector<CTxOut> voutIn)
    {
        if(isSet){return false;}

        BOOST_FOREACH(const CTxIn& in, vinIn)
            sev.push_back(in);

        BOOST_FOREACH(const CTxOut& out, voutIn)
            vout.push_back(out);

        amount = amountIn;
        collateral = collateralIn;
        isSet = true;
        addedTime = GetTime();

        return true;
    }

    bool AddSig(const CTxIn& vin)
    {
        BOOST_FOREACH(CTxDSIn& s, sev) {
            if(s.prevout == vin.prevout && s.nSequence == vin.nSequence){
                if(s.fHasSig){return false;}
                s.scriptSig = vin.scriptSig;
                s.prevPubKey = vin.prevPubKey;
                s.fHasSig = true;

                return true;
            }
        }

        return false;
    }

    bool IsExpired()
    {
        return (GetTime() - addedTime) > DARKSEND_QUEUE_TIMEOUT;// 30 seconds
    }
};


/**
 * A currently inprogress Darksend merge and denomination information
 */
class CDarksendQueue
{
public:
    CTxIn vin;
    int64_t time;
    int nDenom;
    bool ready; //ready for submit
    std::vector<unsigned char> vchSig;

    CDarksendQueue()
    {
        nDenom = 0;
        vin = CTxIn();
        time = 0;
        vchSig.clear();
        ready = false;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(nDenom);
        READWRITE(vin);
        READWRITE(time);
        READWRITE(ready);
        READWRITE(vchSig);
    }

    bool GetAddress(CService &addr)
    {
        CMasternode* pmn = mnodeman.Find(vin);
        if(pmn != NULL)
        {
            addr = pmn->addr;
            return true;
        }
        return false;
    }

    /// Get the protocol version
    bool GetProtocolVersion(int &protocolVersion)
    {
        CMasternode* pmn = mnodeman.Find(vin);
        if(pmn != NULL)
        {
            protocolVersion = pmn->protocolVersion;
            return true;
        }
        return false;
    }

    /** Sign this Darksend transaction
     *  \return true if all conditions are met:
     *     1) we have an active Masternode,
     *     2) we have a valid Masternode private key,
     *     3) we signed the message successfully, and
     *     4) we verified the message successfully
     */
    bool Sign();

    bool Relay();

    /// Is this Darksend expired?
    bool IsExpired()
    {
        return (GetTime() - time) > DARKSEND_QUEUE_TIMEOUT;// 30 seconds
    }

    /// Check if we have a valid Masternode address
    bool CheckSignature();

};

/** Helper class to store Darksend transaction (tx) information.
 */
class CDarksendBroadcastTx
{
public:
    CTransaction tx;
    CTxIn vin;
    vector<unsigned char> vchSig;
    int64_t sigTime;
};

/** Helper object for signing and checking signatures
 */
class CDarkSendSigner
{
public:
    /// Is the inputs associated with this public key? (and there is 1000 VIOG - checking if valid masternode)
    bool IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey);
    /// Set the private/public key values, returns true if successful
    bool SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey);
    /// Sign the message, returns true if successful
    bool SignMessage(std::string strMessage, std::string& errorMessage, std::vector<unsigned char>& vchSig, CKey key);
    /// Verify the message, returns true if succcessful
    bool VerifyMessage(CPubKey pubkey, std::vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage);
};

/** Used to keep track of current status of Darksend pool
 */
class CDarksendPool
{
private:
    mutable CCriticalSection cs_darksend;

    std::vector<CDarkSendEntry> entries; // Masternode/clients entries
    CMutableTransaction finalMutableTransaction; // the finalized transaction ready for signing

    int64_t lastTimeChanged; // last time the 'state' changed, in UTC milliseconds

    unsigned int state; // should be one of the POOL_STATUS_XXX values
    unsigned int entriesCount;
    unsigned int lastEntryAccepted;
    unsigned int countEntriesAccepted;

    std::vector<CTxIn> lockedCoins;

    std::string lastMessage;
    bool unitTest;

    int sessionID;

    int sessionUsers; //N Users have said they'll join
    bool sessionFoundMasternode; //If we've found a compatible Masternode
    std::vector<CTransaction> vecSessionCollateral;

    int cachedLastSuccess;

    int minBlockSpacing; //required blocks between mixes
    CMutableTransaction txCollateral;

    int64_t lastNewBlock;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    std::vector<CAmount> darkSendDenominationsSkipped;

    //debugging data
    std::string strAutoDenomResult;

public:
    enum messages {
        ERR_ALREADY_HAVE,
        ERR_DENOM,
        ERR_ENTRIES_FULL,
        ERR_EXISTING_TX,
        ERR_FEES,
        ERR_INVALID_COLLATERAL,
        ERR_INVALID_INPUT,
        ERR_INVALID_SCRIPT,
        ERR_INVALID_TX,
        ERR_MAXIMUM,
        ERR_MN_LIST,
        ERR_MODE,
        ERR_NON_STANDARD_PUBKEY,
        ERR_NOT_A_MN,
        ERR_QUEUE_FULL,
        ERR_RECENT,
        ERR_SESSION,
        ERR_MISSING_TX,
        ERR_VERSION,
        MSG_NOERR,
        MSG_SUCCESS,
        MSG_ENTRIES_ADDED
    };

    CMasternode* pSubmittedToMasternode;
    int sessionDenom; //Users must submit an denom matching this
    int cachedNumBlocks; //used for the overview screen
    bool fCreateAutoBackups; //builtin support for automatic backups

    CDarksendPool()
    {
        /* Darksend uses collateral addresses to trust parties entering the pool
            to behave themselves. If they don't it takes their money. */

        cachedLastSuccess = 0;
        cachedNumBlocks = std::numeric_limits<int>::max();
        unitTest = false;
        txCollateral = CMutableTransaction();
        minBlockSpacing = 0;
        lastNewBlock = 0;
        fCreateAutoBackups = true;

        SetNull();
    }

    /** Process a Darksend message using the Darksend protocol
     * \param pfrom
     * \param strCommand lower case command string; valid values are:
     *        Command  | Description
     *        -------- | -----------------
     *        dsa      | Darksend Acceptable
     *        dsc      | Darksend Complete
     *        dsf      | Darksend Final tx
     *        dsi      | Darksend vIn
     *        dsq      | Darksend Queue
     *        dss      | Darksend Signal Final Tx
     *        dssu     | Darksend status update
     *        dssub    | Darksend Subscribe To
     * \param vRecv
     */
    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void ClearSkippedDenominations() {
        darkSendDenominationsSkipped.clear();
    }

    bool IsDenomSkipped(CAmount denom) {
        BOOST_FOREACH(CAmount d, darkSendDenominationsSkipped) {
            if (d == denom) {
                return true;
            }
        }
        return false;
    }

    void InitDenominations() {
        darkSendDenominations.clear();
        /* Denominations

           A note about convertability. Within Darksend pools, each denomination
           is convertable to another.

           For example:
           1DRK+1000 == (.1DRK+100)*10
           10DRK+10000 == (1DRK+1000)*10
        */
        darkSendDenominations.push_back( (100      * COIN)+100000 );
        darkSendDenominations.push_back( (10       * COIN)+10000 );
        darkSendDenominations.push_back( (1        * COIN)+1000 );
        darkSendDenominations.push_back( (.1       * COIN)+100 );
        /* Disabled till we need them
        darkSendDenominations.push_back( (.01      * COIN)+10 );
        darkSendDenominations.push_back( (.001     * COIN)+1 );
        */
    }

    void SetMinBlockSpacing(int minBlockSpacingIn){
        minBlockSpacing = minBlockSpacingIn;
    }

    void Reset();
    void SetNull();

    void UnlockCoins();

    bool IsNull() const
    {
        return state == POOL_STATUS_ACCEPTING_ENTRIES && entries.empty();
    }

    int GetState() const
    {
        return state;
    }

    std::string GetStatus();

    int GetEntriesCount() const
    {
        return entries.size();
    }

    /// Get the time the last entry was accepted (time in UTC milliseconds)
    int GetLastEntryAccepted() const
    {
        return lastEntryAccepted;
    }

    /// Get the count of the accepted entries
    int GetCountEntriesAccepted() const
    {
        return countEntriesAccepted;
    }

    // Set the 'state' value, with some logging and capturing when the state changed
    void UpdateState(unsigned int newState)
    {
        if (fMasterNode && (newState == POOL_STATUS_ERROR || newState == POOL_STATUS_SUCCESS)){
            LogPrint("privatesend", "CDarksendPool::UpdateState() - Can't set state to ERROR or SUCCESS as a Masternode. \n");
            return;
        }

        LogPrintf("CDarksendPool::UpdateState() == %d | %d \n", state, newState);
        if(state != newState){
            lastTimeChanged = GetTimeMillis();
            if(fMasterNode) {
                RelayStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);
            }
        }
        state = newState;
    }

    /// Get the maximum number of transactions for the pool
    int GetMaxPoolTransactions()
    {
        return Params().PoolMaxTransactions();
    }

    /// Do we have enough users to take entries?
    bool IsSessionReady(){
        return sessionUsers >= GetMaxPoolTransactions();
    }

    /// Are these outputs compatible with other client in the pool?
    bool IsCompatibleWithEntries(std::vector<CTxOut>& vout);

    /// Is this amount compatible with other client in the pool?
    bool IsCompatibleWithSession(CAmount nAmount, CTransaction txCollateral, int &errorID);

    /// Passively run Darksend in the background according to the configuration in settings
    bool DoAutomaticDenominating(bool fDryRun=false);
    bool PrepareDarksendDenominate();

    /// Check for process in Darksend
    void Check();
    void CheckFinalTransaction();
    /// Charge fees to bad actors (Charge clients a fee if they're abusive)
    void ChargeFees();
    /// Rarely charge fees to pay miners
    void ChargeRandomFees();
    void CheckTimeout();
    void CheckForCompleteQueue();
    /// Check to make sure a signature matches an input in the pool
    bool SignatureValid(const CScript& newSig, const CTxIn& newVin);
    /// If the collateral is valid given by a client
    bool IsCollateralValid(const CTransaction& txCollateral);
    /// Add a clients entry to the pool
    bool AddEntry(const std::vector<CTxIn>& newInput, const CAmount& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, int& errorID);
    /// Add signature to a vin
    bool AddScriptSig(const CTxIn& newVin);
    /// Check that all inputs are signed. (Are all inputs signed?)
    bool SignaturesComplete();
    /// As a client, send a transaction to a Masternode to start the denomination process
    void SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, CAmount amount);
    /// Get Masternode updates about the progress of Darksend
    bool StatusUpdate(int newState, int newEntriesCount, int newAccepted, int &errorID, int newSessionID=0);

    /// As a client, check and sign the final transaction
    bool SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node);

    /// Get the last valid block hash for a given modulus
    bool GetLastValidBlockHash(uint256& hash, int mod=1, int nBlockHeight=0);
    /// Process a new block
    void NewBlock();
    void CompletedTransaction(bool error, int errorID);
    void ClearLastMessage();

    /// Split up large inputs or make fee sized inputs
    bool MakeCollateralAmounts();
    bool CreateDenominated(CAmount nTotalValue);

    /// Get the denominations for a list of outputs (returns a bitshifted integer)
    int GetDenominations(const std::vector<CTxOut>& vout, bool fSingleRandomDenom = false);
    int GetDenominations(const std::vector<CTxDSOut>& vout);

    void GetDenominationsToString(int nDenom, std::string& strDenom);

    /// Get the denominations for a specific amount of vivoinnovaonexgobyte.
    int GetDenominationsByAmount(CAmount nAmount, int nDenomTarget=0); // is not used anymore?
    int GetDenominationsByAmounts(std::vector<CAmount>& vecAmount);

    std::string GetMessageByID(int messageID);

    //
    // Relay Darksend Messages
    //

    void RelayFinalTransaction(const int sessionID, const CTransaction& txNew);
    void RelaySignaturesAnon(std::vector<CTxIn>& vin);
    void RelayInAnon(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout);
    void RelayIn(const std::vector<CTxDSIn>& vin, const CAmount& nAmount, const CTransaction& txCollateral, const std::vector<CTxDSOut>& vout);
    void RelayStatus(const int sessionID, const int newState, const int newEntriesCount, const int newAccepted, const int errorID=MSG_NOERR);
    void RelayCompletedTransaction(const int sessionID, const bool error, const int errorID);

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

void ThreadCheckDarkSendPool();

#endif
