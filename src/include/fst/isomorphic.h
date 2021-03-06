// See www.openfst.org for extensive documentation on this weighted
// finite-state transducer library.
//
// Function to test two FSTs are isomorphic, i.e., they are equal up to a state
// and arc re-ordering. FSTs should be deterministic when viewed as
// unweighted automata.

#ifndef FST_LIB_ISOMORPHIC_H_
#define FST_LIB_ISOMORPHIC_H_

#include <algorithm>

#include <fst/fst.h>


namespace fst {

template <class Arc>
class Isomorphism {
  typedef typename Arc::StateId StateId;
  typedef typename Arc::Weight Weight;

 public:
  Isomorphism(const Fst<Arc> &fst1, const Fst<Arc> &fst2, float delta)
      : fst1_(fst1.Copy()),
        fst2_(fst2.Copy()),
        delta_(delta),
        error_(false),
        comp_(delta, &error_) {}

  // Checks if input FSTs are isomorphic
  bool IsIsomorphic() {
    if (fst1_->Start() == kNoStateId && fst2_->Start() == kNoStateId) {
      return true;
    }

    if (fst1_->Start() == kNoStateId || fst2_->Start() == kNoStateId) {
      return false;
    }

    PairState(fst1_->Start(), fst2_->Start());
    while (!queue_.empty()) {
      const std::pair<StateId, StateId> &pr = queue_.front();
      if (!IsIsomorphicState(pr.first, pr.second)) return false;
      queue_.pop_front();
    }
    return true;
  }

  bool Error() const { return error_; }

 private:
  // Orders arcs for equality checking.
  class ArcCompare {
   public:
    ArcCompare(float delta, bool *error) : delta_(delta), error_(error) {}

    bool operator()(const Arc &arc1, const Arc &arc2) const {
      if (arc1.ilabel < arc2.ilabel) return true;
      if (arc1.ilabel > arc2.ilabel) return false;

      if (arc1.olabel < arc2.olabel) return true;
      if (arc1.olabel > arc2.olabel) return false;

      return WeightCompare(arc1.weight, arc2.weight, delta_, error_);
    }

   private:
    float delta_;
    bool *error_;
  };

  // Orders weights for equality checking.
  static bool WeightCompare(Weight w1, Weight w2, float delta, bool *error);

  // Maintains state correspondences and queue.
  bool PairState(StateId s1, StateId s2) {
    if (state_pairs_.size() <= s1) state_pairs_.resize(s1 + 1, kNoStateId);
    if (state_pairs_[s1] == s2) {
      return true;  // already seen this pair
    } else if (state_pairs_[s1] != kNoStateId) {
      return false;  // s1 already paired with another s2
    }

    state_pairs_[s1] = s2;
    queue_.push_back(std::make_pair(s1, s2));
    return true;
  }

  // Checks if state pair is isomorphic
  bool IsIsomorphicState(StateId s1, StateId s2);

  std::unique_ptr<Fst<Arc>> fst1_;
  std::unique_ptr<Fst<Arc>> fst2_;
  float delta_;                          // Weight equality delta
  std::vector<Arc> arcs1_;               // for sorting arcs on FST1
  std::vector<Arc> arcs2_;               // for sorting arcs on FST2
  std::vector<StateId> state_pairs_;     // maintains state correspondences
  std::list<std::pair<StateId, StateId>> queue_;  // queue of states to process
  bool error_;                           // error flag
  ArcCompare comp_;
};

template <class Arc>
bool Isomorphism<Arc>::WeightCompare(Weight w1, Weight w2, float delta,
                                     bool *error) {
  if (Weight::Properties() & kIdempotent) {
    NaturalLess<Weight> less;
    return less(w1, w2);
  } else {  // No natural order; use hash
    Weight q1 = w1.Quantize(delta);
    Weight q2 = w2.Quantize(delta);
    size_t n1 = q1.Hash();
    size_t n2 = q2.Hash();

    // Hash not unique. Very unlikely to happen.
    if (n1 == n2 && q1 != q2) {
      FSTERROR() << "Isomorphic: Weight hash collision";
      *error = true;
    }
    return n1 < n2;
  }
}

template <class Arc>
bool Isomorphism<Arc>::IsIsomorphicState(StateId s1, StateId s2) {
  if (!ApproxEqual(fst1_->Final(s1), fst2_->Final(s2), delta_)) return false;
  if (fst1_->NumArcs(s1) != fst2_->NumArcs(s2)) return false;

  ArcIterator<Fst<Arc>> aiter1(*fst1_, s1);
  ArcIterator<Fst<Arc>> aiter2(*fst2_, s2);

  arcs1_.clear();
  arcs2_.clear();
  for (; !aiter1.Done(); aiter1.Next(), aiter2.Next()) {
    arcs1_.push_back(aiter1.Value());
    arcs2_.push_back(aiter2.Value());
  }

  std::sort(arcs1_.begin(), arcs1_.end(), comp_);
  std::sort(arcs2_.begin(), arcs2_.end(), comp_);

  Arc arc0;
  for (size_t i = 0; i < arcs1_.size(); ++i) {
    const Arc &arc1 = arcs1_[i];
    const Arc &arc2 = arcs2_[i];

    if (arc1.ilabel != arc2.ilabel) return false;
    if (arc1.olabel != arc2.olabel) return false;
    if (!ApproxEqual(arc1.weight, arc2.weight, delta_)) return false;
    if (!PairState(arc1.nextstate, arc2.nextstate)) return false;

    if (i > 0) {  // Checks for non-determinism
      const Arc &arc0 = arcs1_[i - 1];
      if (arc1.ilabel == arc0.ilabel && arc1.olabel == arc0.olabel &&
          ApproxEqual(arc1.weight, arc0.weight, delta_)) {
        FSTERROR() << "Isomorphic: Non-determinism as an unweighted automaton";
        error_ = true;
        return false;
      }
    }
  }
  return true;
}

// Tests if two Fsts have the same states and arcs up to a reordering.
// Inputs should be non-deterministic when viewed as unweighted automata
// (cf. Encode()).  Returns optional error value (useful when
// FLAGS_error_fatal = false).
template <class Arc>
bool Isomorphic(const Fst<Arc> &fst1, const Fst<Arc> &fst2,
                float delta = kDelta, bool *error = nullptr) {
  Isomorphism<Arc> iso(fst1, fst2, delta);
  bool ret = iso.IsIsomorphic();
  if (iso.Error()) {
    FSTERROR() << "Isomorphic: Can't determine if inputs are isomorphic";
    if (error) *error = true;
    return false;
  } else {
    if (error) *error = false;
    return ret;
  }
}

}  // namespace fst

#endif  // FST_LIB_ISOMORPHIC_H_
