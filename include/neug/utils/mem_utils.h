#pragma once

#include <glog/logging.h>

#include <cstddef>
#include <string>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace neug {

/**
 * @brief Get current process RSS (Resident Set Size) in MB.
 */
inline double GetCurrentRSSMB() {
#ifdef __APPLE__
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS) {
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
  }
  return -1.0;
#else
  std::ifstream stat_file("/proc/self/status");
  std::string line;
  while (std::getline(stat_file, line)) {
    if (line.substr(0, 5) == "VmRSS") {
      std::istringstream iss(line);
      std::string key;
      long value;
      std::string unit;
      iss >> key >> value >> unit;
      return static_cast<double>(value) / 1024.0;  // kB -> MB
    }
  }
  return -1.0;
#endif
}

/**
 * @brief Log memory delta with a tag. Call once to record baseline,
 * call again to log the difference.
 */
class MemTracer {
 public:
  explicit MemTracer(const std::string& tag) : tag_(tag) {
    baseline_ = GetCurrentRSSMB();
    VLOG(1) << "[MemTrace:" << tag_ << "] START RSS=" << baseline_ << " MB";
  }

  double checkpoint(const std::string& label) {
    double current = GetCurrentRSSMB();
    double delta = current - baseline_;
    VLOG(1) << "[MemTrace:" << tag_ << "] " << label
            << " RSS=" << current << " MB, delta=" << delta << " MB";
    return delta;
  }

  double elapsed() const { return GetCurrentRSSMB() - baseline_; }

 private:
  std::string tag_;
  double baseline_;
};

}  // namespace neug
