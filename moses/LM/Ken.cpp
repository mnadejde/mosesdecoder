// $Id$

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include <cstring>
#include <iostream>
#include <memory>
#include <stdlib.h>
#include "lm/binary_format.hh"
#include "lm/enumerate_vocab.hh"
#include "lm/left.hh"
#include "lm/model.hh"

#include "Ken.h"
#include "Base.h"
#include "moses/FFState.h"
#include "moses/TypeDef.h"
#include "moses/Util.h"
#include "moses/FactorCollection.h"
#include "moses/Phrase.h"
#include "moses/InputFileStream.h"
#include "moses/StaticData.h"
#include "moses/ChartHypothesis.h"
#include "moses/Incremental.h"

#include <boost/shared_ptr.hpp>

using namespace std;

namespace Moses {
namespace {

struct KenLMState : public FFState {
  lm::ngram::State state;
  int Compare(const FFState &o) const {
    const KenLMState &other = static_cast<const KenLMState &>(o);
    if (state.length < other.state.length) return -1;
    if (state.length > other.state.length) return 1;
    return std::memcmp(state.words, other.state.words, sizeof(lm::WordIndex) * state.length);
  }
};

class LanguageModelChartStateKenLM : public FFState {
  public:
    LanguageModelChartStateKenLM() {}

    const lm::ngram::ChartState &GetChartState() const { return m_state; }
    lm::ngram::ChartState &GetChartState() { return m_state; }

    int Compare(const FFState& o) const
    {
      const LanguageModelChartStateKenLM &other = static_cast<const LanguageModelChartStateKenLM&>(o);
      int ret = m_state.Compare(other.m_state);
      return ret;
    }

  private:
    lm::ngram::ChartState m_state;
};

class MappingBuilder : public lm::EnumerateVocab {
public:
  MappingBuilder(FactorCollection &factorCollection, std::vector<lm::WordIndex> &mapping)
    : m_factorCollection(factorCollection), m_mapping(mapping) {}

  void Add(lm::WordIndex index, const StringPiece &str) {
    std::size_t factorId = m_factorCollection.AddFactor(str)->GetId();
    if (m_mapping.size() <= factorId) {
      // 0 is <unk> :-)
      m_mapping.resize(factorId + 1);
    }
    m_mapping[factorId] = index;
  }

private:
  FactorCollection &m_factorCollection;
  std::vector<lm::WordIndex> &m_mapping;
};

} // namespace

template <class Model> LanguageModelKen<Model>::LanguageModelKen(const std::string &file, FactorType factorType, bool lazy) : m_factorType(factorType) {
  lm::ngram::Config config;
  IFVERBOSE(1) {
    config.messages = &std::cerr;
  } else {
    config.messages = NULL;
  }
  FactorCollection &collection = FactorCollection::Instance();
  MappingBuilder builder(collection, m_lmIdLookup);
  config.enumerate_vocab = &builder;
  config.load_method = lazy ? util::LAZY : util::POPULATE_OR_READ;

  m_ngram.reset(new Model(file.c_str(), config));

  m_beginSentenceFactor = collection.AddFactor(BOS_);
}

template <class Model> bool LanguageModelKen<Model>::Useable(const Phrase &phrase) const {
  return (phrase.GetSize()>0 && phrase.GetFactor(0, m_factorType) != NULL);
}

template <class Model> std::string LanguageModelKen<Model>::GetScoreProducerDescription(unsigned) const {
  std::ostringstream oss;
  oss << "LM_" << (unsigned)m_ngram->Order() << "gram";
  return oss.str();
}

template <class Model> const FFState *LanguageModelKen<Model>::EmptyHypothesisState(const InputType &/*input*/) const {
  KenLMState *ret = new KenLMState();
  ret->state = m_ngram->BeginSentenceState();
  return ret;
}

template <class Model> lm::WordIndex *LanguageModelKen<Model>::LastIDs(const Hypothesis &hypo, lm::WordIndex *indices) const {
  lm::WordIndex *index = indices;
  lm::WordIndex *end = indices + m_ngram->Order() - 1;
  int position = hypo.GetCurrTargetWordsRange().GetEndPos();
  for (; ; ++index, --position) {
    if (index == end) return index;
    if (position == -1) {
      *index = m_ngram->GetVocabulary().BeginSentence();
      return index + 1;
    }
    *index = TranslateID(hypo.GetWord(position));
  }
}


template <class Model> LanguageModel *LanguageModelKen<Model>::Duplicate() const {
  return new LanguageModelKen<Model>(*this);
}

template <class Model> void LanguageModelKen<Model>::IncrementalCallback(Incremental::Manager &manager) const {
  manager.LMCallback(*m_ngram, m_lmIdLookup);
}

template <class Model> LanguageModelKen<Model>::LanguageModelKen(const LanguageModelKen<Model> &copy_from) :
    m_ngram(copy_from.m_ngram),
    // TODO: don't copy this.      
    m_beginSentenceFactor(copy_from.m_beginSentenceFactor),
    m_factorType(copy_from.m_factorType),
    m_lmIdLookup(copy_from.m_lmIdLookup) {
}

/**
 * Pre-calculate the n-gram probabilities for the words in the specified phrase.
 * 
 * Note that when this method is called, we do not have access to the context
 * in which this phrase will eventually be applied. 
 *
 * In other words, we know what words are in this phrase,
 * but we do not know what words will come before or after this phrase.
 *
 * The parameters fullScore, ngramScore, and oovCount are all output parameters.
 *
 * The value stored in oovCount is the number of words in the phrase
 * that are not in the language model's vocabulary.
 *
 * The sum of the ngram scores for all words in this phrase are stored in fullScore.
 *
 * The value stored in ngramScore is similar, but only full-order ngram scores are included.
 *
 * This is best shown by example:
 * 
 * Assume a trigram language model and a phrase "a b c d e f g"
 *
 * fullScore would represent the sum of the logprob scores for the following values:
 *
 * p(a)
 * p(b | a)
 * p(c | a b)
 * p(d | b c)
 * p(e | c d)
 * p(f | d e)
 * p(g | e f)
 *
 * ngramScore would represent the sum of the logprob scores for the following values:
 *
 * p(c | a b)
 * p(d | b c)
 * p(e | c d)
 * p(f | d e)
 * p(g | e f)
 */
template <class Model> void LanguageModelKen<Model>::CalcScore(const Phrase &phrase, float &fullScore, float &ngramScore, size_t &oovCount) const {
  fullScore = 0;
  ngramScore = 0;
  oovCount = 0;

  if (!phrase.GetSize()) return;

  lm::ngram::ChartState discarded_sadly;
  lm::ngram::RuleScore<Model> scorer(*m_ngram, discarded_sadly);
  
  size_t position;
  if (m_beginSentenceFactor == phrase.GetWord(0).GetFactor(m_factorType)) {
    scorer.BeginSentence();
    position = 1;
  } else {
    position = 0;
  }
  
  size_t ngramBoundary = m_ngram->Order() - 1;

  size_t end_loop = std::min(ngramBoundary, phrase.GetSize());
  for (; position < end_loop; ++position) {
    const Word &word = phrase.GetWord(position);
    if (word.IsNonTerminal()) {
      fullScore += scorer.Finish();
      scorer.Reset();
    } else {
      lm::WordIndex index = TranslateID(word);
      scorer.Terminal(index);
      if (!index) ++oovCount;
    }
  }
  float before_boundary = fullScore + scorer.Finish();
  for (; position < phrase.GetSize(); ++position) {
     const Word &word = phrase.GetWord(position);
    if (word.IsNonTerminal()) {
      fullScore += scorer.Finish();
      scorer.Reset();
    } else {
      lm::WordIndex index = TranslateID(word);
      scorer.Terminal(index);
      if (!index) ++oovCount;
    }   
  }
  fullScore += scorer.Finish();

  ngramScore = TransformLMScore(fullScore - before_boundary);
  fullScore = TransformLMScore(fullScore);
}

/**
 * Calculate the ngram probabilities for the words at the beginning 
 * (and under some circumstances, also at the end)
 * of the phrase represented by the provided hypothesis.
 *
 * Additionally, calculate a new language model state.
 *
 * This is best shown by example:
 *
 * Assume a trigram language model.
 *
 * Assume the previous phrase was "w x y z", 
 * which means the previous language model state is "y z".
 *
 * Assume the provided hypothesis contains the new phrase "a b c d e f g"
 * 
 * Given these assumptions, this method is responsible 
 * for calculating the scores for the following:
 * 
 * p(a | y z)
 * p(b | z a)
 *
 * This method must also calculate and return a new language model state.
 *
 * In this example, the returned language model state would be "f g"
 *
 * If the provided hypothesis represents the end of a completed translation
 * (all source words have been translated)
 * then this method is additionally responsible for calculating the following:
 *
 * p(</s> | f g)
 *
 */
template <class Model> FFState *LanguageModelKen<Model>::Evaluate(const Hypothesis &hypo, const FFState *ps, ScoreComponentCollection *out) const {
  const lm::ngram::State &in_state = static_cast<const KenLMState&>(*ps).state;

  std::auto_ptr<KenLMState> ret(new KenLMState());
  
  if (!hypo.GetCurrTargetLength()) {
    ret->state = in_state;
    return ret.release();
  }

  const std::size_t begin = hypo.GetCurrTargetWordsRange().GetStartPos();
  //[begin, end) in STL-like fashion.
  const std::size_t end = hypo.GetCurrTargetWordsRange().GetEndPos() + 1;
  const std::size_t adjust_end = std::min(end, begin + m_ngram->Order() - 1);

  std::size_t position = begin;
  typename Model::State aux_state;
  typename Model::State *state0 = &ret->state, *state1 = &aux_state;

  float score = m_ngram->Score(in_state, TranslateID(hypo.GetWord(position)), *state0);
  ++position;
  for (; position < adjust_end; ++position) {
    score += m_ngram->Score(*state0, TranslateID(hypo.GetWord(position)), *state1);
    std::swap(state0, state1);
  }

  if (hypo.IsSourceCompleted()) {
    // Score end of sentence.  
    std::vector<lm::WordIndex> indices(m_ngram->Order() - 1);
    const lm::WordIndex *last = LastIDs(hypo, &indices.front());
    score += m_ngram->FullScoreForgotState(&indices.front(), last, m_ngram->GetVocabulary().EndSentence(), ret->state).prob;
  } else if (adjust_end < end) {
    // Get state after adding a long phrase.  
    std::vector<lm::WordIndex> indices(m_ngram->Order() - 1);
    const lm::WordIndex *last = LastIDs(hypo, &indices.front());
    m_ngram->GetState(&indices.front(), last, ret->state);
  } else if (state0 != &ret->state) {
    // Short enough phrase that we can just reuse the state.  
    ret->state = *state0;
  }

  score = TransformLMScore(score);

  if (OOVFeatureEnabled()) {
    std::vector<float> scores(2);
    scores[0] = score;
    scores[1] = 0.0;
    out->PlusEquals(this, scores);
  } else {
    out->PlusEquals(this, score);
  }

  return ret.release();
}

template <class Model> FFState *LanguageModelKen<Model>::EvaluateChart(const ChartHypothesis& hypo, int featureID, ScoreComponentCollection *accumulator) const {
  LanguageModelChartStateKenLM *newState = new LanguageModelChartStateKenLM();
  lm::ngram::RuleScore<Model> ruleScore(*m_ngram, newState->GetChartState());
  const TargetPhrase &target = hypo.GetCurrTargetPhrase();
  const AlignmentInfo::NonTermIndexMap &nonTermIndexMap =
        target.GetAlignNonTerm().GetNonTermIndexMap();

  const size_t size = hypo.GetCurrTargetPhrase().GetSize();
  size_t phrasePos = 0;
  // Special cases for first word.  
  if (size) {
    const Word &word = hypo.GetCurrTargetPhrase().GetWord(0);
    if (word.GetFactor(m_factorType) == m_beginSentenceFactor) {
      // Begin of sentence
      ruleScore.BeginSentence();
      phrasePos++;
    } else if (word.IsNonTerminal()) {
      // Non-terminal is first so we can copy instead of rescoring.  
      const ChartHypothesis *prevHypo = hypo.GetPrevHypo(nonTermIndexMap[phrasePos]);
      const lm::ngram::ChartState &prevState = static_cast<const LanguageModelChartStateKenLM*>(prevHypo->GetFFState(featureID))->GetChartState();
      ruleScore.BeginNonTerminal(prevState, prevHypo->GetScoreBreakdown().GetScoresForProducer(this)[0]);
      phrasePos++;
    }
  }

  for (; phrasePos < size; phrasePos++) {
    const Word &word = hypo.GetCurrTargetPhrase().GetWord(phrasePos);
    if (word.IsNonTerminal()) {
      const ChartHypothesis *prevHypo = hypo.GetPrevHypo(nonTermIndexMap[phrasePos]);
      const lm::ngram::ChartState &prevState = static_cast<const LanguageModelChartStateKenLM*>(prevHypo->GetFFState(featureID))->GetChartState();
      ruleScore.NonTerminal(prevState, prevHypo->GetScoreBreakdown().GetScoresForProducer(this)[0]);
    } else {
      ruleScore.Terminal(TranslateID(word));
    }
  }

  accumulator->Assign(this, ruleScore.Finish());
  return newState;
}

LanguageModel *ConstructKenLM(const std::string &file, FactorType factorType, bool lazy) {
  try {
    lm::ngram::ModelType model_type;
    if (lm::ngram::RecognizeBinary(file.c_str(), model_type)) {
      switch(model_type) {
        case lm::ngram::PROBING:
          return new LanguageModelKen<lm::ngram::ProbingModel>(file,  factorType, lazy);
        case lm::ngram::REST_PROBING:
          return new LanguageModelKen<lm::ngram::RestProbingModel>(file, factorType, lazy);
        case lm::ngram::TRIE:
          return new LanguageModelKen<lm::ngram::TrieModel>(file, factorType, lazy);
        case lm::ngram::QUANT_TRIE:
          return new LanguageModelKen<lm::ngram::QuantTrieModel>(file, factorType, lazy);
        case lm::ngram::ARRAY_TRIE:
          return new LanguageModelKen<lm::ngram::ArrayTrieModel>(file, factorType, lazy);
        case lm::ngram::QUANT_ARRAY_TRIE:
          return new LanguageModelKen<lm::ngram::QuantArrayTrieModel>(file, factorType, lazy);
        default:
          std::cerr << "Unrecognized kenlm model type " << model_type << std::endl;
          abort();
      }
    } else {
      return new LanguageModelKen<lm::ngram::ProbingModel>(file, factorType, lazy);
    }
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
    abort();
  }
}

}

