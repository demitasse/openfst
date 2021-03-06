// See www.openfst.org for extensive documentation on this weighted
// finite-state transducer library.
//
// Class to determine if a non-epsilon label can be read as the first
// non-epsilon symbol along some path from a given state.

#ifndef FST_LIB_LABEL_REACHABLE_H_
#define FST_LIB_LABEL_REACHABLE_H_

#include <unordered_map>
#include <vector>

#include <fst/accumulator.h>
#include <fst/arcsort.h>
#include <fst/interval-set.h>
#include <fst/state-reachable.h>
#include <fst/util.h>
#include <fst/vector-fst.h>


namespace fst {

// Stores shareable data for label reachable class copies.
template <typename L>
class LabelReachableData {
 public:
  typedef L Label;
  typedef IntervalSet<L> LabelIntervalSet;
  typedef typename IntervalSet<L>::Interval Interval;

  explicit LabelReachableData(bool reach_input, bool keep_relabel_data = true)
      : reach_input_(reach_input),
        keep_relabel_data_(keep_relabel_data),
        have_relabel_data_(true),
        final_label_(kNoLabel) {}

  ~LabelReachableData() {}

  bool ReachInput() const { return reach_input_; }

  std::vector<LabelIntervalSet> *MutableIntervalSets() { return &isets_; }

  const LabelIntervalSet &GetIntervalSet(int s) const { return isets_[s]; }

  int NumIntervalSets() const { return isets_.size(); }

  std::unordered_map<L, L> *Label2Index() {
    if (!have_relabel_data_) {
      FSTERROR() << "LabelReachableData: No relabeling data";
    }
    return &label2index_;
  }

  void SetFinalLabel(Label final_label) { final_label_ = final_label; }

  Label FinalLabel() const { return final_label_; }

  static LabelReachableData<L> *Read(std::istream &istrm,
                                     const FstReadOptions &opts) {
    LabelReachableData<L> *data = new LabelReachableData<L>();

    ReadType(istrm, &data->reach_input_);
    ReadType(istrm, &data->keep_relabel_data_);
    data->have_relabel_data_ = data->keep_relabel_data_;
    if (data->keep_relabel_data_) ReadType(istrm, &data->label2index_);
    ReadType(istrm, &data->final_label_);
    ReadType(istrm, &data->isets_);
    return data;
  }

  bool Write(std::ostream &ostrm, const FstWriteOptions &opts) const {
    WriteType(ostrm, reach_input_);
    WriteType(ostrm, keep_relabel_data_);
    if (keep_relabel_data_) WriteType(ostrm, label2index_);
    WriteType(ostrm, FinalLabel());
    WriteType(ostrm, isets_);
    return true;
  }

 private:
  LabelReachableData() {}

  bool reach_input_;               // Input or output labels considered?
  bool keep_relabel_data_;         // Save label2index_ to file?
  bool have_relabel_data_;         // Using label2index_?
  Label final_label_;                     // Final label
  std::unordered_map<L, L> label2index_;  // Finds index for a label.
  std::vector<LabelIntervalSet> isets_;  // Interval sets per state.
};

// Tests reachability of labels from a given state. If reach_input =
// true, then input labels are considered, o.w. output labels are
// considered. To test for reachability from a state s, first do
// SetState(s). Then a label l can be reached from state s of FST f
// iff Reach(r) is true where r = Relabel(l). The relabeling is
// required to ensure a compact representation of the reachable
// labels.

// The whole FST can be relabeled instead with Relabel(&f,
// reach_input) so that the test Reach(r) applies directly to the
// labels of the transformed FST f. The relabeled FST will also be
// sorted appropriately for composition.
//
// Reachablity of a final state from state s (via an epsilon path)
// can be tested with ReachFinal();
//
// Reachability can also be tested on the set of labels specified by
// an arc iterator, useful for FST composition.  In particular,
// Reach(aiter, ...) is true if labels on the input (output) side of
// the transitions of the arc iterator, when iter_input is true
// (false), can be reached from the state s. The iterator labels must
// have already been relabeled.
//
// With the arc iterator test of reachability, the begin position, end
// position and accumulated arc weight of the matches can be
// returned. The optional template argument controls how reachable arc
// weights are accumulated.  The default uses the semiring
// Plus(). Alternative ones can be used to distribute the weights in
// composition in various ways.
template <class A, class S = DefaultAccumulator<A>,
          class D = LabelReachableData<typename A::Label>>
class LabelReachable {
 public:
  typedef A Arc;
  typedef D Data;
  typedef typename A::StateId StateId;
  typedef typename A::Label Label;
  typedef typename A::Weight Weight;
  typedef typename D::LabelIntervalSet LabelIntervalSet;
  typedef typename LabelIntervalSet::Interval Interval;

  LabelReachable(const Fst<A> &fst, bool reach_input, S *s = nullptr,
                 bool keep_relabel_data = true)
      : fst_(new VectorFst<Arc>(fst)),
        s_(kNoStateId),
        data_(std::make_shared<D>(reach_input, keep_relabel_data)),
        accumulator_(s ? s : new S()),
        ncalls_(0),
        nintervals_(0),
        reach_fst_input_(false),
        error_(false) {
    StateId ins = fst_->NumStates();
    TransformFst();
    FindIntervals(ins);
    fst_.reset();
  }

  explicit LabelReachable(std::shared_ptr<D> data, S *s = nullptr)
      : s_(kNoStateId),
        data_(std::move(data)),
        accumulator_(s ? s : new S()),
        ncalls_(0),
        nintervals_(0),
        reach_fst_input_(false),
        error_(false) {}

  LabelReachable(const LabelReachable<A, S, D> &reachable, bool safe = false)
      : s_(kNoStateId),
        data_(reachable.data_),
        accumulator_(new S(*reachable.accumulator_, safe)),
        ncalls_(0),
        nintervals_(0),
        reach_fst_input_(reachable.reach_fst_input_),
        error_(reachable.error_) {}

  ~LabelReachable() {
    if (ncalls_ > 0) {
      VLOG(2) << "# of calls: " << ncalls_;
      VLOG(2) << "# of intervals/call: " << (nintervals_ / ncalls_);
    }
  }

  // Relabels w.r.t labels that give compact label sets.
  Label Relabel(Label label) {
    if (label == 0 || error_) return label;
    std::unordered_map<Label, Label> &label2index = *data_->Label2Index();
    Label &relabel = label2index[label];
    if (!relabel) {  // Add new label
      relabel = label2index.size() + 1;
    }
    return relabel;
  }

  // Relabels Fst w.r.t to labels that give compact label sets.
  void Relabel(MutableFst<Arc> *fst, bool relabel_input) {
    for (StateIterator<MutableFst<Arc>> siter(*fst); !siter.Done();
         siter.Next()) {
      StateId s = siter.Value();
      for (MutableArcIterator<MutableFst<Arc>> aiter(fst, s); !aiter.Done();
           aiter.Next()) {
        Arc arc = aiter.Value();
        if (relabel_input) {
          arc.ilabel = Relabel(arc.ilabel);
        } else {
          arc.olabel = Relabel(arc.olabel);
        }
        aiter.SetValue(arc);
      }
    }
    if (relabel_input) {
      ArcSort(fst, ILabelCompare<Arc>());
      fst->SetInputSymbols(nullptr);
    } else {
      ArcSort(fst, OLabelCompare<Arc>());
      fst->SetOutputSymbols(nullptr);
    }
  }

  // Returns relabeling pairs (cf. relabel.h::Relabel()).
  // If 'avoid_collisions' is true, extra pairs are added to
  // ensure no collisions when relabeling automata that have
  // labels unseen here.
  void RelabelPairs(std::vector<std::pair<Label, Label>> *pairs,
                    bool avoid_collisions = false) {
    pairs->clear();
    const std::unordered_map<Label, Label> &label2index = *data_->Label2Index();
    // Maps labels to their new values in [1, label2index().size()]
    for (auto it = label2index.begin(); it != label2index.end(); ++it) {
      if (it->second != data_->FinalLabel()) {
        pairs->push_back(std::pair<Label, Label>(it->first, it->second));
      }
    }
    if (avoid_collisions) {
      // Ensures any label in [1, label2index().size()] is mapped either
      // by the above step or to label2index() + 1 (to avoid collisions).
      for (int i = 1; i <= label2index.size(); ++i) {
        typename std::unordered_map<Label, Label>::const_iterator it =
            label2index.find(i);
        if (it == label2index.end() || it->second == data_->FinalLabel()) {
          pairs->push_back(std::pair<Label, Label>(i, label2index.size() + 1));
        }
      }
    }
  }

  // Set current state. Optionally set state associated
  // with arc iterator to be passed to Reach.
  void SetState(StateId s, StateId aiter_s = kNoStateId) {
    s_ = s;
    if (aiter_s != kNoStateId) {
      accumulator_->SetState(aiter_s);
      if (accumulator_->Error()) error_ = true;
    }
  }

  // Can reach this label from current state?
  // Original labels must be transformed by the Relabel methods above.
  bool Reach(Label label) const {
    if (label == 0 || error_) return false;
    return data_->GetIntervalSet(s_).Member(label);
  }

  // Can reach final state (via epsilon transitions) from this state?
  bool ReachFinal() const {
    if (error_) return false;
    return data_->GetIntervalSet(s_).Member(data_->FinalLabel());
  }

  // Initialize with secondary FST to be used with Reach(Iterator,...).
  // If reach_input = true, then arc input labels are considered in
  // Reach(aiter, ...), o.w. output labels are considered.
  // If copy is true, then 'fst' is a copy of the FST used in the
  // previous call to this method (useful to avoid unnecessary updates).
  template <class F>
  void ReachInit(const F &fst, bool reach_input, bool copy = false) {
    reach_fst_input_ = reach_input;
    if (!fst.Properties(reach_fst_input_ ? kILabelSorted : kOLabelSorted,
                        true)) {
      FSTERROR() << "LabelReachable::ReachInit: Fst is not sorted";
      error_ = true;
    }
    accumulator_->Init(fst, copy);
    if (accumulator_->Error()) error_ = true;
  }

  // Can reach any arc iterator label between iterator positions
  // aiter_begin and aiter_end?
  // Arc iterator labels must be transformed by the Relabel methods
  // above. If compute_weight is true, user may call ReachWeight().
  template <class Iterator>
  bool Reach(Iterator *aiter, ssize_t aiter_begin, ssize_t aiter_end,
             bool compute_weight) {
    if (error_) return false;
    const LabelIntervalSet& iset = data_->GetIntervalSet(s_);
    ++ncalls_;
    nintervals_ += iset.Size();

    reach_begin_ = -1;
    reach_end_ = -1;
    reach_weight_ = Weight::Zero();

    uint32 flags = aiter->Flags();  // save flags to restore them on exit
    aiter->SetFlags(kArcNoCache, kArcNoCache);  // make caching optional
    aiter->Seek(aiter_begin);

    if (2 * (aiter_end - aiter_begin) < iset.Size()) {
      // Check each arc against intervals.
      // Set arc iterator flags to only compute the ilabel or olabel values,
      // since they are the only values required for most of the arcs processed.
      aiter->SetFlags(reach_fst_input_ ? kArcILabelValue : kArcOLabelValue,
                      kArcValueFlags);
      Label reach_label = kNoLabel;
      for (ssize_t aiter_pos = aiter_begin; aiter_pos < aiter_end;
           aiter->Next(), ++aiter_pos) {
        const A &arc = aiter->Value();
        Label label = reach_fst_input_ ? arc.ilabel : arc.olabel;
        if (label == reach_label || Reach(label)) {
          reach_label = label;
          if (reach_begin_ < 0) reach_begin_ = aiter_pos;
          reach_end_ = aiter_pos + 1;
          if (compute_weight) {
            if (!(aiter->Flags() & kArcWeightValue)) {
              // If the 'arc.weight' wasn't computed by the call
              // to 'aiter->Value()' above, we need to call
              // 'aiter->Value()' again after having set the arc iterator
              // flags to compute the arc weight value.
              aiter->SetFlags(kArcWeightValue, kArcValueFlags);
              const A &arcb = aiter->Value();
              // Call the accumulator.
              reach_weight_ = accumulator_->Sum(reach_weight_, arcb.weight);
              // Only ilabel or olabel required to process the following
              // arcs.
              aiter->SetFlags(
                  reach_fst_input_ ? kArcILabelValue : kArcOLabelValue,
                  kArcValueFlags);
            } else {
              // Call the accumulator.
              reach_weight_ = accumulator_->Sum(reach_weight_, arc.weight);
            }
          }
        }
      }
    } else {
      // Check each interval against arcs
      ssize_t begin_low, end_low = aiter_begin;
      for (const Interval& i : iset) {
        begin_low = LowerBound(aiter, end_low, aiter_end, i.begin);
        end_low = LowerBound(aiter, begin_low, aiter_end, i.end);
        if (end_low - begin_low > 0) {
          if (reach_begin_ < 0) reach_begin_ = begin_low;
          reach_end_ = end_low;
          if (compute_weight) {
            aiter->SetFlags(kArcWeightValue, kArcValueFlags);
            reach_weight_ =
                accumulator_->Sum(reach_weight_, aiter, begin_low, end_low);
          }
        }
      }
    }

    aiter->SetFlags(flags, kArcFlags);  // restore original flag values
    return reach_begin_ >= 0;
  }

  // Returns iterator position of first matching arc.
  ssize_t ReachBegin() const { return reach_begin_; }

  // Returns iterator position one past last matching arc.
  ssize_t ReachEnd() const { return reach_end_; }

  // Return the sum of the weights for matching arcs.
  // Valid only if compute_weight was true in Reach() call.
  Weight ReachWeight() const { return reach_weight_; }

  // Access to the relabeling map. Excludes epsilon (0) label but
  // includes kNoLabel that is used internally for super-final
  // transitons.
  const std::unordered_map<Label, Label> &Label2Index() const {
    return *data_->Label2Index();
  }

  const D *GetData() const { return data_.get(); }

  std::shared_ptr<D> GetSharedData() const { return data_; }

  bool Error() const { return error_ || accumulator_->Error(); }

 private:
  // Redirects labeled arcs (input or output labels determined by
  // ReachInput()) to new label-specific final states.  Each original
  // final state is redirected via a transition labeled with kNoLabel
  // to a new kNoLabel-specific final state.  Creates super-initial
  // state for all states with zero in-degree.
  void TransformFst() {
    StateId ins = fst_->NumStates();
    StateId ons = ins;

    std::vector<ssize_t> indeg(ins, 0);

    // Redirects labeled arcs to new final states.
    for (StateId s = 0; s < ins; ++s) {
      for (MutableArcIterator<VectorFst<Arc>> aiter(fst_.get(), s);
           !aiter.Done(); aiter.Next()) {
        Arc arc = aiter.Value();
        Label label = data_->ReachInput() ? arc.ilabel : arc.olabel;
        if (label) {
          auto insert_result = label2state_.insert(std::make_pair(label, ons));
          if (insert_result.second) {
            indeg.push_back(0);
            ++ons;
          }
          arc.nextstate = label2state_[label];
          aiter.SetValue(arc);
        }
        ++indeg[arc.nextstate];  // Finds in-degrees for next step.
      }

      // Redirects final weights to new final state.
      const Weight final_weight = fst_->Final(s);
      if (final_weight != Weight::Zero()) {
        auto insert_result = label2state_.insert(std::make_pair(kNoLabel, ons));
        if (insert_result.second) {
          indeg.push_back(0);
          ++ons;
        }
        Arc arc(kNoLabel, kNoLabel, final_weight, label2state_[kNoLabel]);
        fst_->AddArc(s, arc);
        ++indeg[arc.nextstate];  // Finds in-degrees for next step.

        fst_->SetFinal(s, Weight::Zero());
      }
    }

    // Add new final states to Fst.
    while (fst_->NumStates() < ons) {
      StateId s = fst_->AddState();
      fst_->SetFinal(s, Weight::One());
    }

    // Creates a super-initial state for all states with zero in-degree.
    StateId start = fst_->AddState();
    fst_->SetStart(start);
    for (StateId s = 0; s < start; ++s) {
      if (indeg[s] == 0) {
        Arc arc(0, 0, Weight::One(), s);
        fst_->AddArc(start, arc);
      }
    }
  }

  void FindIntervals(StateId ins) {
    StateReachable<A, Label, LabelIntervalSet> state_reachable(*fst_);
    if (state_reachable.Error()) {
      error_ = true;
      return;
    }

    std::vector<Label> &state2index = state_reachable.State2Index();
    std::vector<LabelIntervalSet> &isets = *data_->MutableIntervalSets();
    isets = state_reachable.IntervalSets();
    isets.resize(ins);

    std::unordered_map<Label, Label> &label2index = *data_->Label2Index();
    for (const auto &kv : label2state_) {
      Label l = kv.first;
      StateId s = kv.second;
      Label i = state2index[s];
      label2index[l] = i;
      if (l == kNoLabel) {
        data_->SetFinalLabel(i);
      }
    }
    label2state_.clear();

    double nintervals = 0;
    ssize_t non_intervals = 0;
    for (ssize_t s = 0; s < ins; ++s) {
      nintervals += isets[s].Size();
      if (isets[s].Size() > 1) {
        ++non_intervals;
        VLOG(3) << "state: " << s << " # of intervals: " << isets[s].Size();
      }
    }
    VLOG(2) << "# of states: " << ins;
    VLOG(2) << "# of intervals: " << nintervals;
    VLOG(2) << "# of intervals/state: " << nintervals / ins;
    VLOG(2) << "# of non-interval states: " << non_intervals;
  }

  template <class Iterator>
  ssize_t LowerBound(Iterator *aiter, ssize_t aiter_begin, ssize_t aiter_end,
                     Label match_label) const {
    // Only need to compute the ilabel or olabel of arcs when
    // performing the binary search.
    aiter->SetFlags(reach_fst_input_ ? kArcILabelValue : kArcOLabelValue,
                    kArcValueFlags);
    ssize_t low = aiter_begin;
    ssize_t high = aiter_end;
    while (low < high) {
      ssize_t mid = (low + high) / 2;
      aiter->Seek(mid);
      Label label =
          reach_fst_input_ ? aiter->Value().ilabel : aiter->Value().olabel;
      if (label > match_label) {
        high = mid;
      } else if (label < match_label) {
        low = mid + 1;
      } else {
        // Find first matching label (when non-deterministic)
        for (ssize_t i = mid; i > low; --i) {
          aiter->Seek(i - 1);
          label =
              reach_fst_input_ ? aiter->Value().ilabel : aiter->Value().olabel;
          if (label != match_label) {
            aiter->Seek(i);
            aiter->SetFlags(kArcValueFlags, kArcValueFlags);
            return i;
          }
        }
        aiter->SetFlags(kArcValueFlags, kArcValueFlags);
        return low;
      }
    }
    aiter->Seek(low);
    aiter->SetFlags(kArcValueFlags, kArcValueFlags);
    return low;
  }

  std::unique_ptr<VectorFst<Arc>> fst_;
  StateId s_;                             // Current state
  std::unordered_map<Label, StateId>
      label2state_;  // Finds final state for a label

  ssize_t reach_begin_;  // Iterator pos of first match
  ssize_t reach_end_;    // Iterator pos after last match
  Weight reach_weight_;  // Gives weight sum of arc iterator
                         // arcs with reachable labels.
  std::shared_ptr<D> data_;  // Shareable data between copies
  std::unique_ptr<S> accumulator_;  // Sums arc weights

  double ncalls_;
  double nintervals_;
  bool reach_fst_input_;
  bool error_;
};

}  // namespace fst

#endif  // FST_LIB_LABEL_REACHABLE_H_
