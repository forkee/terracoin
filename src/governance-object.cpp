// Copyright (c) 2014-2017 The Terracoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "core_io.h"
#include "darksend.h"
#include "governance.h"
#include "governance-classes.h"
#include "governance-object.h"
#include "governance-vote.h"
#include "masternodeman.h"
#include "util.h"

#include <univalue.h>

CGovernanceObject::CGovernanceObject()
: cs(),
  nObjectType(GOVERNANCE_OBJECT_UNKNOWN),
  nHashParent(),
  nRevision(0),
  nTime(0),
  nDeletionTime(0),
  nCollateralHash(),
  strData(),
  vinMasternode(),
  vchSig(),
  fCachedLocalValidity(false),
  strLocalValidityError(),
  fCachedFunding(false),
  fCachedValid(true),
  fCachedDelete(false),
  fCachedEndorsed(false),
  fDirtyCache(true),
  fExpired(false),
  fUnparsable(false),
  mapCurrentMNVotes(),
  mapOrphanVotes(),
  fileVotes()
{
    // PARSE JSON DATA STORAGE (STRDATA)
    LoadData();
}

CGovernanceObject::CGovernanceObject(uint256 nHashParentIn, int nRevisionIn, int64_t nTimeIn, uint256 nCollateralHashIn, std::string strDataIn)
: cs(),
  nObjectType(GOVERNANCE_OBJECT_UNKNOWN),
  nHashParent(nHashParentIn),
  nRevision(nRevisionIn),
  nTime(nTimeIn),
  nDeletionTime(0),
  nCollateralHash(nCollateralHashIn),
  strData(strDataIn),
  vinMasternode(),
  vchSig(),
  fCachedLocalValidity(false),
  strLocalValidityError(),
  fCachedFunding(false),
  fCachedValid(true),
  fCachedDelete(false),
  fCachedEndorsed(false),
  fDirtyCache(true),
  fExpired(false),
  fUnparsable(false),
  mapCurrentMNVotes(),
  mapOrphanVotes(),
  fileVotes()
{
    // PARSE JSON DATA STORAGE (STRDATA)
    LoadData();
}

CGovernanceObject::CGovernanceObject(const CGovernanceObject& other)
: cs(),
  nObjectType(other.nObjectType),
  nHashParent(other.nHashParent),
  nRevision(other.nRevision),
  nTime(other.nTime),
  nDeletionTime(other.nDeletionTime),
  nCollateralHash(other.nCollateralHash),
  strData(other.strData),
  vinMasternode(other.vinMasternode),
  vchSig(other.vchSig),
  fCachedLocalValidity(other.fCachedLocalValidity),
  strLocalValidityError(other.strLocalValidityError),
  fCachedFunding(other.fCachedFunding),
  fCachedValid(other.fCachedValid),
  fCachedDelete(other.fCachedDelete),
  fCachedEndorsed(other.fCachedEndorsed),
  fDirtyCache(other.fDirtyCache),
  fExpired(other.fExpired),
  fUnparsable(other.fUnparsable),
  mapCurrentMNVotes(other.mapCurrentMNVotes),
  mapOrphanVotes(other.mapOrphanVotes),
  fileVotes(other.fileVotes)
{}

bool CGovernanceObject::ProcessVote(CNode* pfrom,
                                    const CGovernanceVote& vote,
                                    CGovernanceException& exception)
{
    int nMNIndex = governance.GetMasternodeIndex(vote.GetVinMasternode());
    if(nMNIndex < 0) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Masternode index not found\n";
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_WARNING);
        if(mapOrphanVotes.Insert(vote.GetVinMasternode(), vote_time_pair_t(vote, GetAdjustedTime() + GOVERNANCE_ORPHAN_EXPIRATION_TIME))) {
            if(pfrom) {
                mnodeman.AskForMN(pfrom, vote.GetVinMasternode());
            }
            LogPrintf(ostr.str().c_str());
        }
        else {
            LogPrint("gobject", ostr.str().c_str());
        }
        return false;
    }

    vote_m_it it = mapCurrentMNVotes.find(nMNIndex);
    if(it == mapCurrentMNVotes.end()) {
        it = mapCurrentMNVotes.insert(vote_m_t::value_type(nMNIndex,vote_rec_t())).first;
    }
    vote_rec_t& recVote = it->second;
    vote_signal_enum_t eSignal = vote.GetSignal();
    if(eSignal == VOTE_SIGNAL_NONE) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Vote signal: none" << "\n";
        LogPrint("gobject", ostr.str().c_str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_WARNING);
        return false;
    }
    if(eSignal > MAX_SUPPORTED_VOTE_SIGNAL) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Unsupported vote signal:" << CGovernanceVoting::ConvertSignalToString(vote.GetSignal()) << "\n";
        LogPrintf(ostr.str().c_str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        return false;
    }
    vote_instance_m_it it2 = recVote.mapInstances.find(int(eSignal));
    if(it2 == recVote.mapInstances.end()) {
        it2 = recVote.mapInstances.insert(vote_instance_m_t::value_type(int(eSignal), vote_instance_t())).first;
    }
    vote_instance_t& voteInstance = it2->second;

    // Reject obsolete votes
    if(vote.GetTimestamp() < voteInstance.nCreationTime) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Obsolete vote" << "\n";
        LogPrint("gobject", ostr.str().c_str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_NONE);
        return false;
    }

    int64_t nNow = GetTime();
    int64_t nVoteTimeUpdate = voteInstance.nTime;
    if(governance.AreRateChecksEnabled()) {
        int64_t nTimeDelta = nNow - voteInstance.nTime;
        if(nTimeDelta < GOVERNANCE_UPDATE_MIN) {
            std::ostringstream ostr;
            ostr << "CGovernanceObject::ProcessVote -- Masternode voting too often "
                 << ", MN outpoint = " << vote.GetVinMasternode().prevout.ToStringShort()
                 << ", governance object hash = " << GetHash().ToString()
                 << ", time delta = " << nTimeDelta << "\n";
            LogPrint("gobject", ostr.str().c_str());
            exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_TEMPORARY_ERROR);
            nVoteTimeUpdate = nNow;
            return false;
        }
    }
    // Finally check that the vote is actually valid (done last because of cost of signature verification)
    if(!vote.IsValid(true)) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Invalid vote "
                << ", MN outpoint = " << vote.GetVinMasternode().prevout.ToStringShort()
                << ", governance object hash = " << GetHash().ToString()
                << ", vote hash = " << vote.GetHash().ToString() << "\n";
        LogPrintf(ostr.str().c_str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_PERMANENT_ERROR, 20);
        governance.AddInvalidVote(vote);
        return false;
    }
    if(!mnodeman.AddGovernanceVote(vote.GetVinMasternode(), vote.GetParentHash())) {
        std::ostringstream ostr;
        ostr << "CGovernanceObject::ProcessVote -- Unable to add governance vote "
             << ", MN outpoint = " << vote.GetVinMasternode().prevout.ToStringShort()
             << ", governance object hash = " << GetHash().ToString() << "\n";
        LogPrint("gobject", ostr.str().c_str());
        exception = CGovernanceException(ostr.str(), GOVERNANCE_EXCEPTION_PERMANENT_ERROR);
        return false;
    }
    voteInstance = vote_instance_t(vote.GetOutcome(), nVoteTimeUpdate, vote.GetTimestamp());
    if(!fileVotes.HasVote(vote.GetHash())) {
        fileVotes.AddVote(vote);
    }
    fDirtyCache = true;
    return true;
}

void CGovernanceObject::RebuildVoteMap()
{
    vote_m_t mapMNVotesNew;
    for(vote_m_it it = mapCurrentMNVotes.begin(); it != mapCurrentMNVotes.end(); ++it) {
        CTxIn vinMasternode;
        if(mnodeman.GetMasternodeVinForIndexOld(it->first, vinMasternode)) {
            int nNewIndex = mnodeman.GetMasternodeIndex(vinMasternode);
            if((nNewIndex >= 0)) {
                mapMNVotesNew[nNewIndex] = it->second;
            }
        }
    }
    mapCurrentMNVotes = mapMNVotesNew;
}

void CGovernanceObject::ClearMasternodeVotes()
{
    vote_m_it it = mapCurrentMNVotes.begin();
    while(it != mapCurrentMNVotes.end()) {
        bool fIndexRebuilt = false;
        CTxIn vinMasternode;
        bool fRemove = true;
        if(mnodeman.Get(it->first, vinMasternode, fIndexRebuilt)) {
            if(mnodeman.Has(vinMasternode)) {
                fRemove = false;
            }
            else {
                fileVotes.RemoveVotesFromMasternode(vinMasternode);
            }
        }

        if(fRemove) {
            mapCurrentMNVotes.erase(it++);
        }
        else {
            ++it;
        }
    }
}

std::string CGovernanceObject::GetSignatureMessage() const
{
    LOCK(cs);
    std::string strMessage = nHashParent.ToString() + "|" +
        boost::lexical_cast<std::string>(nRevision) + "|" +
        boost::lexical_cast<std::string>(nTime) + "|" +
        strData + "|" +
        vinMasternode.prevout.ToStringShort() + "|" +
        nCollateralHash.ToString();

    return strMessage;
}

void CGovernanceObject::SetMasternodeInfo(const CTxIn& vin)
{
    vinMasternode = vin;
}

bool CGovernanceObject::Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode)
{
    std::string strError;
    std::string strMessage = GetSignatureMessage();

    LOCK(cs);

    if(!darkSendSigner.SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrintf("CGovernanceObject::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)) {
        LogPrintf("CGovernanceObject::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    LogPrint("gobject", "CGovernanceObject::Sign -- pubkey id = %s, vin = %s\n",
             pubKeyMasternode.GetID().ToString(), vinMasternode.prevout.ToStringShort());


    return true;
}

bool CGovernanceObject::CheckSignature(CPubKey& pubKeyMasternode)
{
    std::string strError;

    std::string strMessage = GetSignatureMessage();

    LOCK(cs);
    if(!darkSendSigner.VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)) {
        LogPrintf("CGovernance::CheckSignature -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

int CGovernanceObject::GetObjectSubtype()
{
    // todo - 12.1
    //   - detect subtype from strData json, obj["subtype"]

    if(nObjectType == GOVERNANCE_OBJECT_TRIGGER) return TRIGGER_SUPERBLOCK;
    return -1;
}

uint256 CGovernanceObject::GetHash() const
{
    // CREATE HASH OF ALL IMPORTANT PIECES OF DATA

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nHashParent;
    ss << nRevision;
    ss << nTime;
    ss << strData;
    ss << vinMasternode;
    ss << vchSig;
    // fee_tx is left out on purpose
    uint256 h1 = ss.GetHash();

    DBG( printf("CGovernanceObject::GetHash %i %li %s\n", nRevision, nTime, strData.c_str()); );

    return h1;
}

/**
   Return the actual object from the strData JSON structure.

   Returns an empty object on error.
 */
UniValue CGovernanceObject::GetJSONObject()
{
    UniValue obj(UniValue::VOBJ);
    if(strData.empty()) {
        return obj;
    }

    UniValue objResult(UniValue::VOBJ);
    GetData(objResult);

    std::vector<UniValue> arr1 = objResult.getValues();
    std::vector<UniValue> arr2 = arr1.at( 0 ).getValues();
    obj = arr2.at( 1 );

    return obj;
}

/**
*   LoadData
*   --------------------------------------------------------
*
*   Attempt to load data from strData
*
*/

void CGovernanceObject::LoadData()
{
    // todo : 12.1 - resolved
    //return;

    if(strData.empty()) {
        return;
    }

    try  {
        // ATTEMPT TO LOAD JSON STRING FROM STRDATA
        UniValue objResult(UniValue::VOBJ);
        GetData(objResult);

        DBG( cout << "CGovernanceObject::LoadData strData = "
             << GetDataAsString()
             << endl; );

        UniValue obj = GetJSONObject();
        nObjectType = obj["type"].get_int();
    }
    catch(std::exception& e) {
        fUnparsable = true;
        std::ostringstream ostr;
        ostr << "CGovernanceObject::LoadData Error parsing JSON"
             << ", e.what() = " << e.what();
        DBG( cout << ostr.str() << endl; );
        LogPrintf( ostr.str().c_str() );
        return;
    }
    catch(...) {
        fUnparsable = true;
        std::ostringstream ostr;
        ostr << "CGovernanceObject::LoadData Unknown Error parsing JSON";
        DBG( cout << ostr.str() << endl; );
        LogPrintf( ostr.str().c_str() );
        return;
    }
}

/**
*   GetData - Example usage:
*   --------------------------------------------------------
*
*   Decode governance object data into UniValue(VOBJ)
*
*/

void CGovernanceObject::GetData(UniValue& objResult)
{
    UniValue o(UniValue::VOBJ);
    std::string s = GetDataAsString();
    o.read(s);
    objResult = o;
}

/**
*   GetData - As
*   --------------------------------------------------------
*
*/

std::string CGovernanceObject::GetDataAsHex()
{
    return strData;
}

std::string CGovernanceObject::GetDataAsString()
{
    std::vector<unsigned char> v = ParseHex(strData);
    std::string s(v.begin(), v.end());

    return s;
}

void CGovernanceObject::UpdateLocalValidity()
{
    // THIS DOES NOT CHECK COLLATERAL, THIS IS CHECKED UPON ORIGINAL ARRIVAL
    fCachedLocalValidity = IsValidLocally(strLocalValidityError, false);
};


bool CGovernanceObject::IsValidLocally(std::string& strError, bool fCheckCollateral)
{
    bool fMissingMasternode = false;

    return IsValidLocally(strError, fMissingMasternode, fCheckCollateral);
}

bool CGovernanceObject::IsValidLocally(std::string& strError, bool& fMissingMasternode, bool fCheckCollateral)
{
    fMissingMasternode = false;

    if(fUnparsable) {
        strError = "Object data unparseable";
        return false;
    }

    switch(nObjectType) {
        case GOVERNANCE_OBJECT_PROPOSAL:
        case GOVERNANCE_OBJECT_TRIGGER:
        case GOVERNANCE_OBJECT_WATCHDOG:
            break;
        default:
            strError = strprintf("Invalid object type %d", nObjectType);
            return false;
    }

    // IF ABSOLUTE NO COUNT (NO-YES VALID VOTES) IS MORE THAN 10% OF THE NETWORK MASTERNODES, OBJ IS INVALID

    // CHECK COLLATERAL IF REQUIRED (HIGH CPU USAGE)

    if(fCheckCollateral) { 
        if((nObjectType == GOVERNANCE_OBJECT_TRIGGER) || (nObjectType == GOVERNANCE_OBJECT_WATCHDOG)) {
            std::string strOutpoint = vinMasternode.prevout.ToStringShort();
            masternode_info_t infoMn = mnodeman.GetMasternodeInfo(vinMasternode);
            if(!infoMn.fInfoValid) {
                fMissingMasternode = true;
                strError = "Masternode not found: " + strOutpoint;
                return false;
            }

            // Check that we have a valid MN signature
            if(!CheckSignature(infoMn.pubKeyMasternode)) {
                strError = "Invalid masternode signature for: " + strOutpoint + ", pubkey id = " + infoMn.pubKeyMasternode.GetID().ToString();
                return false;
            }

            return true;
        }

        if(!IsCollateralValid(strError)) {
            // strError set in IsCollateralValid
            if(strError == "") strError = "Collateral is invalid";
            return false;
        }
    }

    /*
        TODO

        - There might be an issue with multisig in the coinbase on mainnet, we will add support for it in a future release.
        - Post 12.2+ (test multisig coinbase transaction)
    */

    // 12.1 - todo - compile error
    // if(address.IsPayToScriptHash()) {
    //     strError = "Governance system - multisig is not currently supported";
    //     return false;
    // }

    return true;
}

CAmount CGovernanceObject::GetMinCollateralFee()
{
    // Only 1 type has a fee for the moment but switch statement allows for future object types
    switch(nObjectType) {
        case GOVERNANCE_OBJECT_PROPOSAL:    return GOVERNANCE_PROPOSAL_FEE_TX;
        case GOVERNANCE_OBJECT_TRIGGER:     return 0;
        case GOVERNANCE_OBJECT_WATCHDOG:    return 0;
        default:                            return MAX_MONEY;
    }
}

bool CGovernanceObject::IsCollateralValid(std::string& strError)
{
    strError = "";
    CAmount nMinFee = GetMinCollateralFee();
    uint256 nExpectedHash = GetHash();

    CTransaction txCollateral;
    uint256 nBlockHash;

    // RETRIEVE TRANSACTION IN QUESTION

    if(!GetTransaction(nCollateralHash, txCollateral, Params().GetConsensus(), nBlockHash, true)){
        strError = strprintf("Can't find collateral tx %s", txCollateral.ToString());
        LogPrintf("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    if(txCollateral.vout.size() < 1) {
        strError = strprintf("tx vout size less than 1 | %d", txCollateral.vout.size());
        LogPrintf("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    // LOOK FOR SPECIALIZED GOVERNANCE SCRIPT

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    LogPrint("gobject", "IsCollateralValid txCollateral.vout.size() = %d, nMinFee = %lu, findScript = %s\n",
        txCollateral.vout.size(), nMinFee, ScriptToAsmStr( findScript, false ));

    bool foundOpReturn = false;
    BOOST_FOREACH(const CTxOut o, txCollateral.vout) {
        DBG( cout << "IsCollateralValid txout : " << o.ToString()
             << ", o.nValue = " << o.nValue
             << ", o.scriptPubKey = " << ScriptToAsmStr( o.scriptPubKey, false )
             << endl; );

        if(!o.scriptPubKey.IsNormalPaymentScript() && !o.scriptPubKey.IsUnspendable()){
            strError = strprintf("Invalid Script %s", txCollateral.ToString());
            LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);
            return false;
        }

        if(o.scriptPubKey == findScript) {
            DBG( cout << "IsCollateralValid foundOpReturn = true" << endl; );
            foundOpReturn = true;
        }
        else  {
            DBG( cout << "IsCollateralValid No match, continuing" << endl; );
        }

    }

    if(!foundOpReturn){
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral.ToString());
        LogPrintf ("CGovernanceObject::IsCollateralValid -- %s\n", strError);
        return false;
    }

    // UNLIKE IN DASH, WE DON'T BURN THE COLLATERAL

    CAmount nValueIn = 0;
    CAmount nValueOut = 0;
    bool fMissingTx = false;

    BOOST_FOREACH(const CTxOut txout, txCollateral.vout) {
        nValueOut += txout.nValue;
    }

    BOOST_FOREACH(const CTxIn txin, txCollateral.vin) {
        CTransaction txPrev;
        uint256 hash;
        if(GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hash, true)) {
            if(txPrev.vout.size() > txin.prevout.n)
                nValueIn += txPrev.vout[txin.prevout.n].nValue;
        } else {
            fMissingTx = true;
        }
    }

    if(fMissingTx) {
        LogPrintf ("CGovernanceObject::IsCollateralValid -- Unknown inputs in collateral transaction, txCollateral=%s", txCollateral.ToString());
        return false;
    }

    if(nValueOut > nValueIn || (nValueIn - nValueOut) < nMinFee)
    {
         LogPrintf ("CGovernanceObject::IsCollateralValid Collateral fee too low txFee = %lu nMinFee = %lu\n",  (nValueIn - nValueOut), nMinFee);
         return false;
    }

    // GET CONFIRMATIONS FOR TRANSACTION

    LOCK(cs_main);
    int nConfirmationsIn = GetIXConfirmations(nCollateralHash);
    if (nBlockHash != uint256()) {
        BlockMap::iterator mi = mapBlockIndex.find(nBlockHash);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                nConfirmationsIn += chainActive.Height() - pindex->nHeight + 1;
            }
        }
    }

    if(nConfirmationsIn >= GOVERNANCE_FEE_CONFIRMATIONS) {
        strError = "valid";
    } else {
        strError = strprintf("Collateral requires at least %d confirmations - %d confirmations", GOVERNANCE_FEE_CONFIRMATIONS, nConfirmationsIn);
        LogPrintf ("CGovernanceObject::IsCollateralValid -- %s - %d confirmations\n", strError, nConfirmationsIn);
        return false;
    }

    return true;
}

int CGovernanceObject::CountMatchingVotes(vote_signal_enum_t eVoteSignalIn, vote_outcome_enum_t eVoteOutcomeIn) const
{
    int nCount = 0;
    for(vote_m_cit it = mapCurrentMNVotes.begin(); it != mapCurrentMNVotes.end(); ++it) {
        const vote_rec_t& recVote = it->second;
        vote_instance_m_cit it2 = recVote.mapInstances.find(eVoteSignalIn);
        if(it2 == recVote.mapInstances.end()) {
            continue;
        }
        const vote_instance_t& voteInstance = it2->second;
        if(voteInstance.eOutcome == eVoteOutcomeIn) {
            ++nCount;
        }
    }
    return nCount;
}

/**
*   Get specific vote counts for each outcome (funding, validity, etc)
*/

int CGovernanceObject::GetAbsoluteYesCount(vote_signal_enum_t eVoteSignalIn) const
{
    return GetYesCount(eVoteSignalIn) - GetNoCount(eVoteSignalIn);
}

int CGovernanceObject::GetAbsoluteNoCount(vote_signal_enum_t eVoteSignalIn) const
{
    return GetNoCount(eVoteSignalIn) - GetYesCount(eVoteSignalIn);
}

int CGovernanceObject::GetYesCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_YES);
}

int CGovernanceObject::GetNoCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_NO);
}

int CGovernanceObject::GetAbstainCount(vote_signal_enum_t eVoteSignalIn) const
{
    return CountMatchingVotes(eVoteSignalIn, VOTE_OUTCOME_ABSTAIN);
}

bool CGovernanceObject::GetCurrentMNVotes(const CTxIn& mnCollateralOutpoint, vote_rec_t& voteRecord)
{
    int nMNIndex = governance.GetMasternodeIndex(mnCollateralOutpoint);
    vote_m_it it = mapCurrentMNVotes.find(nMNIndex);
    if (it == mapCurrentMNVotes.end()) {
        return false;
    }
    voteRecord = it->second;
    return  true;
}

void CGovernanceObject::Relay()
{
    CInv inv(MSG_GOVERNANCE_OBJECT, GetHash());
    RelayInv(inv, PROTOCOL_VERSION);
}

void CGovernanceObject::UpdateSentinelVariables()
{
    // CALCULATE MINIMUM SUPPORT LEVELS REQUIRED

    int nMnCount = mnodeman.CountEnabled();
    if(nMnCount == 0) return;

    // CALCULATE THE MINUMUM VOTE COUNT REQUIRED FOR FULL SIGNAL

    int nAbsVoteReq, nAbsDeleteReq;
    if(Params().NetworkIDString() == CBaseChainParams::MAIN)
    {
        nAbsVoteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, nMnCount / 10);
        nAbsDeleteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, (2 * nMnCount) / 3);
    }
    else
    {
        nAbsVoteReq = Params().GetConsensus().nGovernanceMinQuorum;
        nAbsDeleteReq = Params().GetConsensus().nGovernanceMinQuorum;
    }

    // SET SENTINEL FLAGS TO FALSE

    fCachedFunding = false;
    fCachedValid = true; //default to valid
    fCachedEndorsed = false;
    fDirtyCache = false;

    LogPrint("gobject", "CGovernanceObject::UpdateSentinelVariables %s %d %d %d %d (%d %d)\n",
	GetHash().ToString(), GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING), GetAbsoluteYesCount(VOTE_SIGNAL_DELETE),
        GetAbsoluteYesCount(VOTE_SIGNAL_ENDORSED), GetAbsoluteNoCount(VOTE_SIGNAL_VALID), nAbsVoteReq, nAbsDeleteReq);

    // SET SENTINEL FLAGS TO TRUE IF MIMIMUM SUPPORT LEVELS ARE REACHED
    // ARE ANY OF THESE FLAGS CURRENTLY ACTIVATED?

    if(GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING) >= nAbsVoteReq) fCachedFunding = true;
    if((GetAbsoluteYesCount(VOTE_SIGNAL_DELETE) >= nAbsDeleteReq) && !fCachedDelete) {
        fCachedDelete = true;
        if(nDeletionTime == 0) {
            nDeletionTime = GetAdjustedTime();
        }
    }
    if(GetAbsoluteYesCount(VOTE_SIGNAL_ENDORSED) >= nAbsVoteReq) fCachedEndorsed = true;

    if(GetAbsoluteNoCount(VOTE_SIGNAL_VALID) >= nAbsVoteReq) fCachedValid = false;
}

void CGovernanceObject::swap(CGovernanceObject& first, CGovernanceObject& second) // nothrow
{
    // enable ADL (not necessary in our case, but good practice)
    using std::swap;

    // by swapping the members of two classes,
    // the two classes are effectively swapped
    swap(first.nHashParent, second.nHashParent);
    swap(first.nRevision, second.nRevision);
    swap(first.nTime, second.nTime);
    swap(first.nDeletionTime, second.nDeletionTime);
    swap(first.nCollateralHash, second.nCollateralHash);
    swap(first.strData, second.strData);
    swap(first.nObjectType, second.nObjectType);

    // swap all cached valid flags
    swap(first.fCachedFunding, second.fCachedFunding);
    swap(first.fCachedValid, second.fCachedValid);
    swap(first.fCachedDelete, second.fCachedDelete);
    swap(first.fCachedEndorsed, second.fCachedEndorsed);
    swap(first.fDirtyCache, second.fDirtyCache);
    swap(first.fExpired, second.fExpired);
}

void CGovernanceObject::CheckOrphanVotes()
{
    int64_t nNow = GetAdjustedTime();
    const vote_mcache_t::list_t& listVotes = mapOrphanVotes.GetItemList();
    vote_mcache_t::list_cit it = listVotes.begin();
    while(it != listVotes.end()) {
        bool fRemove = false;
        const CTxIn& key = it->key;
        const vote_time_pair_t& pairVote = it->value;
        const CGovernanceVote& vote = pairVote.first;
        if(pairVote.second < nNow) {
            fRemove = true;
        }
        else if(!mnodeman.Has(vote.GetVinMasternode())) {
            ++it;
            continue;
        }
        CGovernanceException exception;
        if(!ProcessVote(NULL, vote, exception)) {
            LogPrintf("CGovernanceObject::CheckOrphanVotes -- Failed to add orphan vote: %s\n", exception.what());
        }
        else {
            vote.Relay();
            fRemove = true;
        }
        ++it;
        if(fRemove) {
            mapOrphanVotes.Erase(key, pairVote);
        }
    }
}
