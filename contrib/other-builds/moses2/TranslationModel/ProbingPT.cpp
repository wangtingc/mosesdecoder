/*
 * ProbingPT.cpp
 *
 *  Created on: 3 Nov 2015
 *      Author: hieu
 */
#include <boost/foreach.hpp>
#include "ProbingPT.h"
#include "util/exception.hh"
#include "../System.h"
#include "../Scores.h"
#include "../Phrase.h"
#include "../legacy/InputFileStream.h"
#include "../legacy/FactorCollection.h"
#include "../legacy/ProbingPT/quering.hh"
#include "../legacy/Util2.h"
#include "../legacy/ProbingPT/probing_hash_utils.hh"
#include "../FF/FeatureFunctions.h"
#include "../PhraseBased/PhraseImpl.h"
#include "../PhraseBased/TargetPhraseImpl.h"
#include "../PhraseBased/Manager.h"
#include "../PhraseBased/TargetPhrases.h"
#include "../SCFG/InputPath.h"
#include "../SCFG/Manager.h"
#include "../SCFG/TargetPhraseImpl.h"

using namespace std;

namespace Moses2
{
void ProbingPT::ActiveChartEntryProbing::AddSymbolBindElement(
    const Range &range,
    const SCFG::Word &word,
    const Moses2::HypothesisColl *hypos,
    const Moses2::PhraseTable &pt)
{
  const ProbingPT &probingPt = static_cast<const ProbingPT&>(pt);
  std::pair<bool, uint64_t> key = GetKey(word, probingPt);
  UTIL_THROW_IF2(!key.first, "Word should have been in source vocab");
  m_key = key.second;

  ActiveChartEntry::AddSymbolBindElement(range, word, hypos, pt);
}

std::pair<bool, uint64_t> ProbingPT::ActiveChartEntryProbing::GetKey(const SCFG::Word &nextWord, const ProbingPT &pt) const
{
  std::pair<bool, uint64_t> ret;
  ret.second = m_key;
  uint64_t probingId = pt.GetSourceProbingId(nextWord);
  if (probingId == pt.GetUnk()) {
    ret.first = false;
    return ret;
  }

  ret.first = true;
  size_t phraseSize = m_symbolBind.coll.size();
  ret.second += probingId << phraseSize;
  return ret;
}

////////////////////////////////////////////////////////////////////////////
ProbingPT::ProbingPT(size_t startInd, const std::string &line) :
    PhraseTable(startInd, line)
{
  ReadParameters();
}

ProbingPT::~ProbingPT()
{
  delete m_engine;
}

void ProbingPT::Load(System &system)
{
  m_engine = new QueryEngine(m_path.c_str());

  m_unkId = 456456546456;

  FactorCollection &vocab = system.GetVocab();

  // source vocab
  const std::map<uint64_t, std::string> &sourceVocab =
      m_engine->getSourceVocab();
  std::map<uint64_t, std::string>::const_iterator iterSource;
  for (iterSource = sourceVocab.begin(); iterSource != sourceVocab.end();
      ++iterSource) {
    string wordStr = iterSource->second;
    bool isNT;
    //cerr << "wordStr=" << wordStr << endl;
    ReformatWord(system, wordStr, isNT);
    //cerr << "wordStr=" << wordStr << endl;

    const Factor *factor = vocab.AddFactor(wordStr, system, isNT);

    uint64_t probingId = iterSource->first;
    size_t factorId = factor->GetId();

    if (factorId >= m_sourceVocab.size()) {
      m_sourceVocab.resize(factorId + 1, m_unkId);
    }
    m_sourceVocab[factorId] = probingId;
  }

  // target vocab
  InputFileStream targetVocabStrme(m_path + "/TargetVocab.dat");
  string line;
  while (getline(targetVocabStrme, line)) {
    vector<string> toks = Tokenize(line, "\t");
    UTIL_THROW_IF2(toks.size() != 2, string("Incorrect format:") + line + "\n");

    bool isNT;
    //cerr << "wordStr=" << toks[0] << endl;
    ReformatWord(system, toks[0], isNT);
    //cerr << "wordStr=" << toks[0] << endl;

    const Factor *factor = vocab.AddFactor(toks[0], system, isNT);
    uint32_t probingId = Scan<uint32_t>(toks[1]);

    if (probingId >= m_targetVocab.size()) {
      m_targetVocab.resize(probingId + 1, NULL);
    }
    m_targetVocab[probingId] = factor;
  }

  // memory mapped file to tps
  string filePath = m_path + "/TargetColl.dat";
  file.open(filePath.c_str());
  if (!file.is_open()) {
    throw "Couldn't open file ";
  }

  data = file.data();
  //size_t size = file.size();

  // cache
  CreateCache(system);
}

void ProbingPT::Lookup(const Manager &mgr, InputPathsBase &inputPaths) const
{
  BOOST_FOREACH(InputPathBase *pathBase, inputPaths){
  InputPath *path = static_cast<InputPath*>(pathBase);
  TargetPhrases *tpsPtr;
  tpsPtr = Lookup(mgr, mgr.GetPool(), *path);
  path->AddTargetPhrases(*this, tpsPtr);
}
}

TargetPhrases* ProbingPT::Lookup(const Manager &mgr, MemPool &pool,
    InputPath &inputPath) const
{
  /*
   if (inputPath.prefixPath && inputPath.prefixPath->GetTargetPhrases(*this) == NULL) {
   // assume all paths have prefixes, except rules with 1 word source
   return NULL;
   }
   else {
   const Phrase &sourcePhrase = inputPath.subPhrase;
   std::pair<TargetPhrases*, uint64_t> tpsAndKey = CreateTargetPhrase(pool, mgr.system, sourcePhrase);
   return tpsAndKey.first;
   }
   */
  const Phrase<Moses2::Word> &sourcePhrase = inputPath.subPhrase;

  // get hash for source phrase
  std::pair<bool, uint64_t> keyStruct = GetKey(sourcePhrase);
  if (!keyStruct.first) {
    return NULL;
  }

  // check in cache
  Cache::const_iterator iter = m_cache.find(keyStruct.second);
  if (iter != m_cache.end()) {
    TargetPhrases *tps = iter->second;
    return tps;
  }

  // query pt
  TargetPhrases *tps = CreateTargetPhrase(pool, mgr.system, sourcePhrase,
      keyStruct.second);
  return tps;
}

std::pair<bool, uint64_t> ProbingPT::GetKey(const Phrase<Moses2::Word> &sourcePhrase) const
{
  std::pair<bool, uint64_t> ret;

  // create a target phrase from the 1st word of the source, prefix with 'ProbingPT:'
  size_t sourceSize = sourcePhrase.GetSize();
  assert(sourceSize);

  uint64_t probingSource[sourceSize];
  GetSourceProbingIds(sourcePhrase, ret.first, probingSource);
  if (!ret.first) {
    // source phrase contains a word unknown in the pt.
    // We know immediately there's no translation for it
  }
  else {
    ret.second = m_engine->getKey(probingSource, sourceSize);
  }

  return ret;

}

TargetPhrases *ProbingPT::CreateTargetPhrase(MemPool &pool,
    const System &system, const Phrase<Moses2::Word> &sourcePhrase, uint64_t key) const
{
  TargetPhrases *tps = NULL;

  //Actual lookup
  std::pair<bool, uint64_t> query_result; // 1st=found, 2nd=target file offset
  query_result = m_engine->query(key);

  if (query_result.first) {
    const char *offset = data + query_result.second;
    uint64_t *numTP = (uint64_t*) offset;

    tps = new (pool.Allocate<TargetPhrases>()) TargetPhrases(pool, *numTP);

    offset += sizeof(uint64_t);
    for (size_t i = 0; i < *numTP; ++i) {
      TargetPhrase<Moses2::Word> *tp = CreateTargetPhrase(pool, system, offset);
      assert(tp);
      const FeatureFunctions &ffs = system.featureFunctions;
      ffs.EvaluateInIsolation(pool, system, sourcePhrase, *tp);

      tps->AddTargetPhrase(*tp);

    }

    tps->SortAndPrune(m_tableLimit);
    system.featureFunctions.EvaluateAfterTablePruning(pool, *tps, sourcePhrase);
    //cerr << *tps << endl;
  }

  return tps;
}

TargetPhrase<Moses2::Word> *ProbingPT::CreateTargetPhrase(
    MemPool &pool,
    const System &system,
    const char *&offset) const
{
  TargetPhraseInfo *tpInfo = (TargetPhraseInfo*) offset;
  TargetPhraseImpl *tp =
      new (pool.Allocate<TargetPhraseImpl>()) TargetPhraseImpl(pool, *this,
          system, tpInfo->numWords);

  offset += sizeof(TargetPhraseInfo);

  // scores
  SCORE *scores = (SCORE*) offset;

  size_t totalNumScores = m_engine->num_scores + m_engine->num_lex_scores;

  if (m_engine->logProb) {
    // set pt score for rule
    tp->GetScores().PlusEquals(system, *this, scores);

    // save scores for other FF, eg. lex RO. Just give the offset
    if (m_engine->num_lex_scores) {
      tp->scoreProperties = scores + m_engine->num_scores;
    }
  }
  else {
    // log score 1st
    SCORE logScores[totalNumScores];
    for (size_t i = 0; i < totalNumScores; ++i) {
      logScores[i] = FloorScore(TransformScore(scores[i]));
    }

    // set pt score for rule
    tp->GetScores().PlusEquals(system, *this, logScores);

    // save scores for other FF, eg. lex RO.
    tp->scoreProperties = pool.Allocate<SCORE>(m_engine->num_lex_scores);
    for (size_t i = 0; i < m_engine->num_lex_scores; ++i) {
      tp->scoreProperties[i] = logScores[i + m_engine->num_scores];
    }
  }

  offset += sizeof(SCORE) * totalNumScores;

  // words
  for (size_t i = 0; i < tpInfo->numWords; ++i) {
    uint32_t *probingId = (uint32_t*) offset;

    const Factor *factor = GetTargetFactor(*probingId);
    assert(factor);

    Word &word = (*tp)[i];
    word[0] = factor;

    offset += sizeof(uint32_t);
  }

  // properties TODO

  return tp;
}

void ProbingPT::GetSourceProbingIds(const Phrase<Moses2::Word> &sourcePhrase,
    bool &ok, uint64_t probingSource[]) const
{

  size_t size = sourcePhrase.GetSize();
  for (size_t i = 0; i < size; ++i) {
    const Word &word = sourcePhrase[i];
    uint64_t probingId = GetSourceProbingId(word);
    if (probingId == m_unkId) {
      ok = false;
      return;
    }
    else {
      probingSource[i] = probingId;
    }
  }

  ok = true;
}

uint64_t ProbingPT::GetSourceProbingId(const Word &word) const
{
  const Factor *factor = word[0];

  size_t factorId = factor->GetId();
  if (factorId >= m_sourceVocab.size()) {
    return m_unkId;
  }
  return m_sourceVocab[factorId];

}

void ProbingPT::CreateCache(System &system)
{
  if (m_maxCacheSize == 0) {
    return;
  }

  string filePath = m_path + "/cache";
  InputFileStream strme(filePath);

  string line;
  getline(strme, line);
  //float totalCount = Scan<float>(line);

  MemPool &pool = system.GetSystemPool();
  FactorCollection &vocab = system.GetVocab();

  size_t lineCount = 0;
  while (getline(strme, line) && lineCount < m_maxCacheSize) {
    vector<string> toks = Tokenize(line, "\t");
    assert(toks.size() == 2);
    PhraseImpl *sourcePhrase = PhraseImpl::CreateFromString(pool, vocab, system,
        toks[1]);

    std::pair<bool, uint64_t> retStruct = GetKey(*sourcePhrase);
    if (!retStruct.first) {
      return;
    }

    TargetPhrases *tps = CreateTargetPhrase(pool, system, *sourcePhrase,
        retStruct.second);
    assert(tps);

    m_cache[retStruct.second] = tps;

    ++lineCount;
  }

}

void ProbingPT::ReformatWord(System &system, std::string &wordStr, bool &isNT)
{
  isNT = false;
  if (system.isPb) {
    return;
  }
  else {
    isNT = (wordStr[0] == '[' && wordStr[wordStr.size() - 1] == ']');
    //cerr << "nt=" << nt << endl;

    if (isNT) {
      size_t startPos = wordStr.find("][");
      if (startPos == string::npos) {
        startPos = 1;
      }
      else {
        startPos += 2;
      }

      wordStr = wordStr.substr(startPos, wordStr.size() - startPos - 1);
      //cerr << "wordStr=" << wordStr << endl;
    }
  }
}

/////////////////////////////////////////////////////////////////////
// SCFG
/////////////////////////////////////////////////////////////////////
void ProbingPT::InitActiveChart(MemPool &pool, SCFG::InputPath &path) const
{
  size_t ptInd = GetPtInd();
  ActiveChartEntryProbing *chartEntry = new (pool.Allocate<ActiveChartEntryProbing>()) ActiveChartEntryProbing(pool);
  path.AddActiveChartEntry(ptInd, chartEntry);
  //cerr << "InitActiveChart=" << path << endl;
}

void ProbingPT::Lookup(MemPool &pool,
    const SCFG::Manager &mgr,
    size_t maxChartSpan,
    const SCFG::Stacks &stacks,
    SCFG::InputPath &path) const
{
}

/*
void ProbingPT::Lookup(MemPool &pool,
    const SCFG::Manager &mgr,
    size_t maxChartSpan,
    const SCFG::Stacks &stacks,
    SCFG::InputPath &path) const
{
  if (path.range.GetNumWordsCovered() > maxChartSpan) {
    return;
  }

  size_t endPos = path.range.GetEndPos();

  const SCFG::InputPath *prevPath = static_cast<const SCFG::InputPath*>(path.prefixPath);
  UTIL_THROW_IF2(prevPath == NULL, "prefixPath == NULL");

  // TERMINAL
  const SCFG::Word &lastWord = path.subPhrase.Back();

  const SCFG::InputPath &subPhrasePath = *mgr.GetInputPaths().GetMatrix().GetValue(endPos, 1);

  //cerr << "BEFORE LookupGivenWord=" << *prevPath << endl;
  LookupGivenWord(pool, mgr, *prevPath, lastWord, NULL, subPhrasePath.range, path);
  //cerr << "AFTER LookupGivenWord=" << *prevPath << endl;

  // NON-TERMINAL
  //const SCFG::InputPath *prefixPath = static_cast<const SCFG::InputPath*>(path.prefixPath);
  while (prevPath) {
    const Range &prevRange = prevPath->range;
    //cerr << "prevRange=" << prevRange << endl;

    size_t startPos = prevRange.GetEndPos() + 1;
    size_t ntSize = endPos - startPos + 1;
    const SCFG::InputPath &subPhrasePath = *mgr.GetInputPaths().GetMatrix().GetValue(startPos, ntSize);

    LookupNT(pool, mgr, subPhrasePath.range, *prevPath, stacks, path);

    prevPath = static_cast<const SCFG::InputPath*>(prevPath->prefixPath);
  }
}
*/
void ProbingPT::LookupUnary(MemPool &pool,
    const SCFG::Manager &mgr,
    const SCFG::Stacks &stacks,
    SCFG::InputPath &path) const
{
  cerr << "LookupUnary" << endl;
  size_t startPos = path.range.GetStartPos();
  const SCFG::InputPath *prevPath = mgr.GetInputPaths().GetMatrix().GetValue(startPos, 0);
  LookupNT(pool, mgr, path.range, *prevPath, stacks, path);
}

void ProbingPT::LookupNT(
    MemPool &pool,
    const SCFG::Manager &mgr,
    const Moses2::Range &subPhraseRange,
    const SCFG::InputPath &prevPath,
    const SCFG::Stacks &stacks,
    SCFG::InputPath &outPath) const
{
  size_t endPos = outPath.range.GetEndPos();

  const Range &prevRange = prevPath.range;

  size_t startPos = prevRange.GetEndPos() + 1;
  size_t ntSize = endPos - startPos + 1;

  const SCFG::Stack &ntStack = stacks.GetStack(startPos, ntSize);
  const SCFG::Stack::Coll &stackColl = ntStack.GetColl();

  BOOST_FOREACH (const SCFG::Stack::Coll::value_type &valPair, stackColl) {
    const SCFG::Word &ntSought = valPair.first;
    const Moses2::HypothesisColl *hypos = valPair.second;
    //cerr << "ntSought=" << ntSought << ntSought.isNonTerminal << endl;
    LookupGivenWord(pool, mgr, prevPath, ntSought, hypos, subPhraseRange, outPath);
  }
}

void ProbingPT::LookupGivenWord(
    MemPool &pool,
    const SCFG::Manager &mgr,
    const SCFG::InputPath &prevPath,
    const SCFG::Word &wordSought,
    const Moses2::HypothesisColl *hypos,
    const Moses2::Range &subPhraseRange,
    SCFG::InputPath &outPath) const
{
  size_t ptInd = GetPtInd();


  BOOST_FOREACH(const SCFG::ActiveChartEntry *prevEntry, *prevPath.GetActiveChart(ptInd).entries) {
    const ActiveChartEntryProbing *prevEntryCast = static_cast<const ActiveChartEntryProbing*>(prevEntry);
    //cerr << "entry=" << &entryCast->node << endl;

    //cerr << "BEFORE LookupGivenNode=" << prevPath << endl;
    LookupGivenNode(pool, mgr, *prevEntryCast, wordSought, hypos, subPhraseRange, outPath);
    //cerr << "AFTER LookupGivenNode=" << prevPath << endl;
  }
}

void ProbingPT::LookupGivenNode(
    MemPool &pool,
    const SCFG::Manager &mgr,
    const ActiveChartEntryProbing &prevEntry,
    const SCFG::Word &wordSought,
    const Moses2::HypothesisColl *hypos,
    const Moses2::Range &subPhraseRange,
    SCFG::InputPath &outPath) const
{
  std::pair<bool, uint64_t> key = prevEntry.GetKey(wordSought, *this);

  if (!key.first) {
    // should only happen when looking up unary rules
    return;
  }

  std::pair<bool, uint64_t> query_result; // 1st=found, 2nd=target file offset
  query_result = m_engine->query(key.second);

  if (query_result.first) {
    size_t ptInd = GetPtInd();
    const FeatureFunctions &ffs = mgr.system.featureFunctions;

    const char *offset = data + query_result.second;
    uint64_t *numTP = (uint64_t*) offset;

    const Phrase<SCFG::Word> &sourcePhrase = outPath.subPhrase;

    SCFG::TargetPhrases *tps = NULL;
    tps = new (pool.Allocate<SCFG::TargetPhrases>()) SCFG::TargetPhrases(pool, *numTP);

    offset += sizeof(uint64_t);
    for (size_t i = 0; i < *numTP; ++i) {
      SCFG::TargetPhraseImpl *tp = CreateTargetPhraseSCFG(pool, mgr.system, offset);
      assert(tp);
      ffs.EvaluateInIsolation(pool, mgr.system, sourcePhrase, *tp);

      tps->AddTargetPhrase(*tp);

    }

    tps->SortAndPrune(m_tableLimit);
    ffs.EvaluateAfterTablePruning(pool, *tps, sourcePhrase);
    //cerr << *tps << endl;

    // there are some rules
    //AddTargetPhrasesToPath(pool, *nextNode, symbolBind, outPath);

    // new entries
    ActiveChartEntryProbing *chartEntry = new (pool.Allocate<ActiveChartEntryProbing>()) ActiveChartEntryProbing(pool, prevEntry);

    chartEntry->AddSymbolBindElement(subPhraseRange, wordSought, hypos, *this);
    //cerr << "AFTER Add=" << symbolBind << endl;

    outPath.AddActiveChartEntry(ptInd, chartEntry);

  }
}

SCFG::TargetPhraseImpl *ProbingPT::CreateTargetPhraseSCFG(
    MemPool &pool,
    const System &system,
    const char *&offset) const
{
  TargetPhraseInfo *tpInfo = (TargetPhraseInfo*) offset;
  SCFG::TargetPhraseImpl *tp =
      new (pool.Allocate<SCFG::TargetPhraseImpl>()) SCFG::TargetPhraseImpl(pool, *this,
          system, tpInfo->numWords - 1);

  offset += sizeof(TargetPhraseInfo);

  // scores
  SCORE *scores = (SCORE*) offset;

  size_t totalNumScores = m_engine->num_scores + m_engine->num_lex_scores;

  if (m_engine->logProb) {
    // set pt score for rule
    tp->GetScores().PlusEquals(system, *this, scores);

    // save scores for other FF, eg. lex RO. Just give the offset
    if (m_engine->num_lex_scores) {
      tp->scoreProperties = scores + m_engine->num_scores;
    }
  }
  else {
    // log score 1st
    SCORE logScores[totalNumScores];
    for (size_t i = 0; i < totalNumScores; ++i) {
      logScores[i] = FloorScore(TransformScore(scores[i]));
    }

    // set pt score for rule
    tp->GetScores().PlusEquals(system, *this, logScores);

    // save scores for other FF, eg. lex RO.
    tp->scoreProperties = pool.Allocate<SCORE>(m_engine->num_lex_scores);
    for (size_t i = 0; i < m_engine->num_lex_scores; ++i) {
      tp->scoreProperties[i] = logScores[i + m_engine->num_scores];
    }
  }

  offset += sizeof(SCORE) * totalNumScores;

  // words
  for (size_t i = 0; i < tpInfo->numWords - 1; ++i) {
    uint32_t *probingId = (uint32_t*) offset;

    const Factor *factor = GetTargetFactor(*probingId);
    assert(factor);

    SCFG::Word &word = (*tp)[i];
    word[0] = factor;

    offset += sizeof(uint32_t);
  }

  // lhs
  uint32_t *probingId = (uint32_t*) offset;

  const Factor *factor = GetTargetFactor(*probingId);
  assert(factor);

  tp->lhs[0] = factor;

  offset += sizeof(uint32_t);

  // properties TODO

  return tp;
}

} // namespace

