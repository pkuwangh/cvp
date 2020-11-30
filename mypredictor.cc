#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "cvp.h"
#include "mypredictor.h"

namespace wangh {

ImemInst::ImemInst(const DynInst::Handle& dynamic_inst) {
  pc_str_ = dynamic_inst->getPcStr();
  inst_class_ = dynamic_inst->inst_class_;
  is_candidate_ = dynamic_inst->is_candidate_;
  dst_reg_ = dynamic_inst->dst_reg_;
  src_regs_ = dynamic_inst->src_regs_;
  count_ = 0;
  total_latency_ = 0;
  last_value_ = 0xdeadbeef;
}

void ImemInst::addDynInst(const DynInst::Handle& dynamic_inst) {
  count_ += 1;
  total_latency_ += dynamic_inst->latency_;
  if (is_candidate_) {
    addValueToMap(cache_hit_map_, dynamic_inst->cache_hit_);
    addValueToMap(value_map_, dynamic_inst->value_);
    if (last_value_ != 0xdeadbeef) {
      const auto stride = dynamic_inst->value_ - last_value_;
      addValueToMap(stride_map_, stride);
    }
    last_value_ = dynamic_inst->value_;
  }
}

std::string ImemInst::getDisplayStr() const {
  // lambda to return a string of N whitespaces
  auto getSpaceStr = [](int n) -> std::string {
    std::string res = std::string(n, ' ');
    return res;
  };
  std::stringstream ss;
  ss << pc_str_ << getSpaceStr(2);
  ss << std::setw(4) << std::left;
  switch (inst_class_) {
    case 0: ss << "alu"; break;
    case 1: ss << "ld"; break;
    case 2: ss << "st"; break;
    case 3: ss << "br"; break;
    case 4: ss << "jmp"; break;
    case 5: ss << "jr"; break;
    case 6: ss << "fp"; break;
    case 7: ss << "mul"; break;
    default: ss << "???"; break;
  }
  if (dst_reg_ != 0xdeadbeef) {
    ss << "r" << std::setw(2) << std::left << dst_reg_ << " <- ";
  } else {
    ss << getSpaceStr(7);
  }
  for (const auto& reg : src_regs_) {
    if (reg != 0xdeadbeef) {
      std::stringstream rss;
      rss << "r" << reg << ",";
      ss << std::setw(4) << std::left << rss.str();
    } else {
      ss << getSpaceStr(4);
    }
  }
  ss << std::setw(6) << std::right << (total_latency_ / count_);
  // stride & value pattern
  using pattern_dist = std::unordered_map<uint64_t, uint64_t>;
  auto get_pattern = [this](const pattern_dist& dist, bool stride) -> std::string {
    using pattern_item = std::pair<uint64_t, uint64_t>;
    std::vector<pattern_item> patterns(dist.begin(), dist.end());
    std::sort(
      patterns.begin(), patterns.end(),
      [](const pattern_item& a, const pattern_item& b) -> bool {
        return a.second > b.second;
      }
    );
    std::stringstream pss;
    const uint32_t top_n = 4;
    uint64_t eff_count = stride ? (count_ - 1) : count_;
    for (size_t i = 0; i < top_n; ++i) {
      if (i < patterns.size()) {
        if (stride) {
          const int64_t stride_val = static_cast<int64_t>(patterns[i].first);
          if (stride_val < 0) {
            pss << "-0x" << std::hex << (-stride_val) << "/";
          } else {
            pss << "0x" << std::hex << stride_val << "/";
          }
        } else {
          pss << "0x" << std::hex << patterns[i].first << "/";
        }
        pss << std::dec << patterns[i].second << "/";
        pss << (100 * patterns[i].second / eff_count) << "%";
      } else {
        pss << "-";
      }
      if (i < top_n - 1) {
        pss << ", ";
      }
    }
    return pss.str();
  };
  ss << getSpaceStr(2) << "[" << get_pattern(value_map_, false) << "]";
  ss << getSpaceStr(2) << "[" << get_pattern(stride_map_, true) << "]";
  return ss.str();
}

void InstTracker::addInstIssue(const uint64_t& seq_no,
    const uint64_t& pc,
    const uint32_t& piece,
    bool is_candidate,
    uint32_t cache_hit) {
  if (inflight_insts_.count(seq_no) > 0) {
    std::cout << "seqnum=" << seq_no << " again???" << std::endl;
  } else {
    DynInst::Handle dynamic_inst = std::make_shared<DynInst>(
        pc, piece, is_candidate, cache_hit);
    inflight_insts_[seq_no] = dynamic_inst;
  }
}

void InstTracker::addInstExec(const uint64_t& seq_no,
    uint32_t inst_class,
    const uint64_t& src1,
    const uint64_t& src2,
    const uint64_t& src3,
    const uint64_t& dst) {
  if (inflight_insts_.count(seq_no) == 0) {
    std::cout << "seqnum=" << seq_no << " gone???" << std::endl;
  } else {
    inflight_insts_[seq_no]->setMetaInfo(inst_class, src1, src2, src3, dst);
  }
}

void InstTracker::addInstRetire(const uint64_t& seq_no,
    const uint64_t& addr,
    const uint64_t& value,
    const uint64_t& latency) {
  total_count_ += 1;
  if (inflight_insts_.count(seq_no) == 0) {
    std::cout << "seqnum=" << seq_no << " gone at retire???" << std::endl;
  } else {
    auto& dynamic_inst = inflight_insts_[seq_no];
    dynamic_inst->setFinalInfo(addr, value, latency);
    if (tracked_insts_.count(dynamic_inst->getPcStr()) == 0) {
      ImemInst::Handle imem_inst = std::make_shared<ImemInst>(dynamic_inst);
      tracked_insts_[dynamic_inst->getPcStr()] = imem_inst;
    }
    tracked_insts_[dynamic_inst->getPcStr()]->addDynInst(dynamic_inst);
  }
  // remove flushed insts
  for (uint64_t i = last_retired_seq_no_; i < seq_no; ++i) {
    if (inflight_insts_.count(i) > 0) {
      inflight_insts_.erase(i);
    }
  }
  last_retired_seq_no_ = seq_no;
}

void InstTracker::dumpImem() {
  uint64_t run_count = 0;
  uint32_t skipped_lines = 0;
  bool skipping = false;
  std::cout << "================ IMEM Start ================" << std::endl;
  for (const auto& x : tracked_insts_) {
    const ImemInst::Handle& inst = x.second;
    run_count += inst->count_;
    const float weight = 100.0 * inst->count_ / total_count_;
    const float run_weight = 100.0 * run_count / total_count_;
    if (weight < 0.01) {
      if (!skipping) {
        std::cout << " ... skipped ";
        skipping = true;
      }
      skipped_lines += 1;
      continue;
    } else {
      if (skipping) {
        std::cout << skipped_lines << " lines." << std::endl;
        skipped_lines = 0;
        skipping = false;
      }
    }
    std::cout << std::setw(8) << inst->count_;
    std::cout << std::fixed;
    std::cout << std::setw(7) << std::setprecision(3) << weight << "%";
    std::cout << std::setw(7) << std::setprecision(2) << run_weight << "%";
    std::cout << std::defaultfloat << "  ";
    std::cout << inst->getDisplayStr() << std::endl;
  }
  std::cout << "================ IMEM End ================" << std::endl;
}

}

// global structures
// ---------------- for analysis only ----------------
static wangh::InstTracker inst_tracker_;
// ---------------- real hardware ----------------

// ---------------- ---------------- ----------------

PredictionResult getPrediction(const PredictionRequest& req) {
  inst_tracker_.addInstIssue(
      req.seq_no, req.pc, req.piece, req.is_candidate,
      static_cast<uint32_t>(req.cache_hit));
  PredictionResult res;
  res.predicted_value = 0x0;
  res.speculate = true;
  return res;
}

void speculativeUpdate(uint64_t seq_no,
    bool eligible,
    uint8_t prediction_result,
    uint64_t pc,
    uint64_t next_pc,
    InstClass insn,
    uint8_t piece,
    uint64_t src1,
    uint64_t src2,
    uint64_t src3,
    uint64_t dst) {
  inst_tracker_.addInstExec(
      seq_no, static_cast<uint32_t>(insn), src1, src2, src3, dst);
}

void updatePredictor(uint64_t seq_no,
    uint64_t actual_addr,
    uint64_t actual_value,
    uint64_t actual_latency) {
  inst_tracker_.addInstRetire(
      seq_no, actual_addr, actual_value, actual_latency);
}

void beginPredictor(int argc_other, char **argv_other) {
  if (argc_other > 0) {
    std::cout << "CONTESTANT ARGUMENTS:" << std::endl;
  }

  for (int i = 0; i < argc_other; i++) {
    std::cout << "\targv_other[" << i << "] = " << argv_other[i];
    std::cout << std::endl;
  }
}

void endPredictor() {
  inst_tracker_.dumpImem();
}
