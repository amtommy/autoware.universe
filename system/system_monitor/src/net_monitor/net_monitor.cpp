// Copyright 2020 Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file net_monitor.cpp
 * @brief Net monitor class
 */

#include "system_monitor/net_monitor/net_monitor.hpp"

#include "system_monitor/system_monitor_utility.hpp"

#include <traffic_reader/traffic_reader.hpp>

#include <boost/archive/text_iarchive.hpp>
#include <boost/range/algorithm.hpp>
// #include <boost/algorithm/string.hpp>   // workaround for build errors

#include <fmt/format.h>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <string>
#include <vector>

NetMonitor::NetMonitor(const rclcpp::NodeOptions & options)
: Node("net_monitor", options),
  updater_(this),
  last_update_time_{0, 0, this->get_clock()->get_clock_type()},
  device_params_(
    declare_parameter<std::vector<std::string>>("devices", std::vector<std::string>())),
  monitor_program_(declare_parameter<std::string>("monitor_program", "greengrass")),
  traffic_reader_port_(declare_parameter<int>("traffic_reader_port", TRAFFIC_READER_PORT)),
  crc_error_check_duration_(declare_parameter<int>("crc_error_check_duration", 1)),
  crc_error_count_threshold_(declare_parameter<int>("crc_error_count_threshold", 1))
{
  using namespace std::literals::chrono_literals;

  if (monitor_program_.empty()) {
    monitor_program_ = GET_ALL_STR;
    nethogs_all_ = true;
  } else {
    nethogs_all_ = false;
  }

  gethostname(hostname_, sizeof(hostname_));
  updater_.setHardwareID(hostname_);
  updater_.add("Network Usage", this, &NetMonitor::checkUsage);
  updater_.add("Network Traffic", this, &NetMonitor::monitorTraffic);
  updater_.add("Network CRC Error", this, &NetMonitor::checkCrcError);

  nl80211_.init();

  // get Network information for the first time
  updateNetworkInfoList();

  timer_ = rclcpp::create_timer(this, get_clock(), 1s, std::bind(&NetMonitor::onTimer, this));
}

NetMonitor::~NetMonitor() { shutdown_nl80211(); }

void NetMonitor::shutdown_nl80211() { nl80211_.shutdown(); }

void NetMonitor::onTimer() { updateNetworkInfoList(); }

void NetMonitor::updateNetworkInfoList()
{
  net_info_list_.clear();

  if (device_params_.empty()) {
    return;
  }

  const struct ifaddrs * ifa;
  struct ifaddrs * ifas = nullptr;

  rclcpp::Duration duration = this->now() - last_update_time_;

  // Get network interfaces
  getifaddrs_errno_ = 0;
  if (getifaddrs(&ifas) < 0) {
    getifaddrs_errno_ = errno;
    return;
  }

  for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
    // Skip no addr
    if (!ifa->ifa_addr) {
      continue;
    }
    // Skip loopback
    if (ifa->ifa_flags & IFF_LOOPBACK) {
      continue;
    }
    // Skip non AF_PACKET
    if (ifa->ifa_addr->sa_family != AF_PACKET) {
      continue;
    }
    // Skip device not specified
    if (
      boost::find(device_params_, ifa->ifa_name) == device_params_.end() &&
      boost::find(device_params_, "*") == device_params_.end()) {
      continue;
    }

    int fd;
    struct ifreq ifrm;
    struct ifreq ifrc;
    struct ethtool_cmd edata;

    net_info_list_.emplace_back();
    auto & net_info = net_info_list_.back();

    net_info.interface_name = std::string(ifa->ifa_name);

    // Get MTU information
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    strncpy(ifrm.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFMTU, &ifrm) < 0) {
      net_info.mtu_errno = errno;
      close(fd);
      continue;
    }

    // Get network capacity
    strncpy(ifrc.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
    ifrc.ifr_data = (caddr_t)&edata;
    edata.cmd = ETHTOOL_GSET;
    if (ioctl(fd, SIOCETHTOOL, &ifrc) < 0) {
      // possibly wireless connection, get bitrate(MBit/s)
      net_info.speed = nl80211_.getBitrate(ifa->ifa_name);
      if (net_info.speed <= 0) {
        net_info.ethtool_errno = errno;
        close(fd);
        continue;
      }
    } else {
      net_info.speed = edata.speed;
    }

    net_info.is_running = (ifa->ifa_flags & IFF_RUNNING);

    auto * stats = (struct rtnl_link_stats *)ifa->ifa_data;
    if (bytes_.find(net_info.interface_name) != bytes_.end()) {
      net_info.rx_traffic =
        toMbit(stats->rx_bytes - bytes_[net_info.interface_name].rx_bytes) / duration.seconds();
      net_info.tx_traffic =
        toMbit(stats->tx_bytes - bytes_[net_info.interface_name].tx_bytes) / duration.seconds();
      net_info.rx_usage = net_info.rx_traffic / net_info.speed;
      net_info.tx_usage = net_info.tx_traffic / net_info.speed;
    }

    net_info.mtu = ifrm.ifr_mtu;
    net_info.rx_bytes = stats->rx_bytes;
    net_info.rx_errors = stats->rx_errors;
    net_info.tx_bytes = stats->tx_bytes;
    net_info.tx_errors = stats->tx_errors;
    net_info.collisions = stats->collisions;

    close(fd);

    bytes_[net_info.interface_name].rx_bytes = stats->rx_bytes;
    bytes_[net_info.interface_name].tx_bytes = stats->tx_bytes;

    // Get the count of CRC errors
    crc_errors & crc_ers = crc_errors_[net_info.interface_name];
    crc_ers.errors_queue.push_back(stats->rx_crc_errors - crc_ers.last_rx_crc_errors);
    while (crc_ers.errors_queue.size() > crc_error_check_duration_) {
      crc_ers.errors_queue.pop_front();
    }
    crc_ers.last_rx_crc_errors = stats->rx_crc_errors;
  }

  freeifaddrs(ifas);

  last_update_time_ = this->now();
}

void NetMonitor::checkUsage(diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  // Remember start time to measure elapsed time
  const auto t_start = SystemMonitorUtility::startMeasurement();

  if (!checkGeneralInfo(stat)) {
    return;
  }

  int level = DiagStatus::OK;
  int whole_level = DiagStatus::OK;
  int index = 0;
  std::string error_str;
  std::vector<std::string> interface_names;

  for (const auto & net_info : net_info_list_) {
    if (!isSupportedNetwork(net_info, index, stat, error_str)) {
      ++index;
      interface_names.push_back(net_info.interface_name);
      continue;
    }

    level = net_info.is_running ? DiagStatus::OK : DiagStatus::ERROR;

    stat.add(fmt::format("Network {}: status", index), usage_dict_.at(level));
    stat.add(fmt::format("Network {}: interface name", index), net_info.interface_name);
    stat.addf(fmt::format("Network {}: rx_usage", index), "%.2f%%", net_info.rx_usage * 1e+2);
    stat.addf(fmt::format("Network {}: tx_usage", index), "%.2f%%", net_info.tx_usage * 1e+2);
    stat.addf(fmt::format("Network {}: rx_traffic", index), "%.2f MBit/s", net_info.rx_traffic);
    stat.addf(fmt::format("Network {}: tx_traffic", index), "%.2f MBit/s", net_info.tx_traffic);
    stat.addf(fmt::format("Network {}: capacity", index), "%.1f MBit/s", net_info.speed);
    stat.add(fmt::format("Network {}: mtu", index), net_info.mtu);
    stat.add(fmt::format("Network {}: rx_bytes", index), net_info.rx_bytes);
    stat.add(fmt::format("Network {}: rx_errors", index), net_info.rx_errors);
    stat.add(fmt::format("Network {}: tx_bytes", index), net_info.tx_bytes);
    stat.add(fmt::format("Network {}: tx_errors", index), net_info.tx_errors);
    stat.add(fmt::format("Network {}: collisions", index), net_info.collisions);

    ++index;

    interface_names.push_back(net_info.interface_name);
  }

  // Check if specified device exists
  for (const auto & device : device_params_) {
    // Skip if all devices specified
    if (device == "*") {
      continue;
    }
    // Skip if device already appended
    if (boost::find(interface_names, device) != interface_names.end()) {
      continue;
    }

    stat.add(fmt::format("Network {}: status", index), "No Such Device");
    stat.add(fmt::format("Network {}: interface name", index), device);
    error_str = "no such device";
    ++index;
  }

  if (!error_str.empty()) {
    stat.summary(DiagStatus::ERROR, error_str);
  } else {
    stat.summary(whole_level, usage_dict_.at(whole_level));
  }

  // Measure elapsed time since start time and report
  SystemMonitorUtility::stopMeasurement(t_start, stat);
}

void NetMonitor::checkCrcError(diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  // Remember start time to measure elapsed time
  const auto t_start = SystemMonitorUtility::startMeasurement();

  if (!checkGeneralInfo(stat)) {
    return;
  }

  int whole_level = DiagStatus::OK;
  int index = 0;
  std::string error_str;

  for (const auto & net_info : net_info_list_) {
    if (!isSupportedNetwork(net_info, index, stat, error_str)) {
      ++index;
      continue;
    }

    crc_errors & crc_ers = crc_errors_[net_info.interface_name];
    unsigned int unit_rx_crc_errors = 0;

    for (auto errors : crc_ers.errors_queue) {
      unit_rx_crc_errors += errors;
    }

    stat.add(fmt::format("Network {}: interface name", index), net_info.interface_name);
    stat.add(fmt::format("Network {}: total rx_crc_errors", index), crc_ers.last_rx_crc_errors);
    stat.add(fmt::format("Network {}: rx_crc_errors per unit time", index), unit_rx_crc_errors);

    if (unit_rx_crc_errors >= crc_error_count_threshold_) {
      whole_level = std::max(whole_level, static_cast<int>(DiagStatus::WARN));
      error_str = "CRC error";
    }

    ++index;
  }

  if (!error_str.empty()) {
    stat.summary(whole_level, error_str);
  } else {
    stat.summary(whole_level, "OK");
  }

  // Measure elapsed time since start time and report
  SystemMonitorUtility::stopMeasurement(t_start, stat);
}

bool NetMonitor::checkGeneralInfo(diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  if (device_params_.empty()) {
    stat.summary(DiagStatus::ERROR, "invalid device parameter");
    return false;
  }

  if (getifaddrs_errno_ != 0) {
    stat.summary(DiagStatus::ERROR, "getifaddrs error");
    stat.add("getifaddrs", strerror(getifaddrs_errno_));
    return false;
  }
  return true;
}

bool NetMonitor::isSupportedNetwork(
  const NetworkInfo & net_info, int index, diagnostic_updater::DiagnosticStatusWrapper & stat,
  std::string & error_str)
{
  // MTU information
  if (net_info.mtu_errno != 0) {
    if (net_info.mtu_errno == ENOTSUP) {
      stat.add(fmt::format("Network {}: status", index), "Not Supported");
    } else {
      stat.add(fmt::format("Network {}: status", index), "Error");
      error_str = "ioctl error";
    }

    stat.add(fmt::format("Network {}: interface name", index), net_info.interface_name);
    stat.add("ioctl(SIOCGIFMTU)", strerror(net_info.mtu_errno));
    return false;
  }

  // network capacity
  if (net_info.speed <= 0) {
    if (net_info.ethtool_errno == ENOTSUP) {
      stat.add(fmt::format("Network {}: status", index), "Not Supported");
    } else {
      stat.add(fmt::format("Network {}: status", index), "Error");
      error_str = "ioctl error";
    }

    stat.add(fmt::format("Network {}: interface name", index), net_info.interface_name);
    stat.add("ioctl(SIOCETHTOOL)", strerror(net_info.ethtool_errno));
    return false;
  }
  return true;
}

#include <boost/algorithm/string.hpp>  // workaround for build errors

void NetMonitor::monitorTraffic(diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  // Remember start time to measure elapsed time
  const auto t_start = SystemMonitorUtility::startMeasurement();

  // Create a new socket
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    stat.summary(DiagStatus::ERROR, "socket error");
    stat.add("socket", strerror(errno));
    return;
  }

  // Specify the receiving timeouts until reporting an error
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  int ret = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (ret < 0) {
    stat.summary(DiagStatus::ERROR, "setsockopt error");
    stat.add("setsockopt", strerror(errno));
    close(sock);
    return;
  }

  // Connect the socket referred to by the file descriptor
  sockaddr_in addr;
  memset(&addr, 0, sizeof(sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(traffic_reader_port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0) {
    stat.summary(DiagStatus::ERROR, "connect error");
    stat.add("connect", strerror(errno));
    close(sock);
    return;
  }

  // Write list of devices to FD
  ret = write(sock, monitor_program_.c_str(), monitor_program_.length());
  if (ret < 0) {
    stat.summary(DiagStatus::ERROR, "write error");
    stat.add("write", strerror(errno));
    RCLCPP_ERROR(get_logger(), "write error");
    close(sock);
    return;
  }

  // Receive messages from a socket
  std::string rcv_str;
  char buf[16 * 1024 + 1];
  do {
    ret = recv(sock, buf, sizeof(buf) - 1, 0);
    if (ret < 0) {
      stat.summary(DiagStatus::ERROR, "recv error");
      stat.add("recv", strerror(errno));
      close(sock);
      return;
    }
    if (ret > 0) {
      buf[ret] = '\0';
      rcv_str += std::string(buf);
    }
  } while (ret > 0);

  // Close the file descriptor FD
  ret = close(sock);
  if (ret < 0) {
    stat.summary(DiagStatus::ERROR, "close error");
    stat.add("close", strerror(errno));
    return;
  }

  // No data received
  if (rcv_str.length() == 0) {
    stat.summary(DiagStatus::ERROR, "recv error");
    stat.add("recv", "No data received");
    return;
  }

  // Restore  information list
  TrafficReaderResult result;
  try {
    std::istringstream iss(rcv_str);
    boost::archive::text_iarchive oa(iss);
    oa >> result;
  } catch (const std::exception & e) {
    stat.summary(DiagStatus::ERROR, "recv error");
    stat.add("recv", e.what());
    return;
  }

  // traffic_reader result to output
  if (result.error_code_ != 0) {
    stat.summary(DiagStatus::ERROR, "traffic_reader error");
    stat.add("error", result.str_);
  } else {
    stat.summary(DiagStatus::OK, "OK");

    if (result.str_.length() == 0) {
      stat.add("nethogs: result", "nothing");
    } else if (nethogs_all_) {
      stat.add("nethogs: all (KB/sec):", result.str_);
    } else {
      std::stringstream lines{result.str_};
      std::string line;
      std::vector<std::string> list;
      int idx = 0;
      while (std::getline(lines, line)) {
        if (line.empty()) {
          continue;
        }
        boost::split(list, line, boost::is_any_of("\t"), boost::token_compress_on);
        if (list.size() >= 3) {
          stat.add(fmt::format("nethogs {}: PROGRAM", idx), list[0].c_str());
          stat.add(fmt::format("nethogs {}: SENT (KB/sec)", idx), list[1].c_str());
          stat.add(fmt::format("nethogs {}: RECEIVED (KB/sec)", idx), list[2].c_str());
        } else {
          stat.add(fmt::format("nethogs {}: result", idx), line);
        }
        idx++;
      }  // while
    }
  }

  // Measure elapsed time since start time and report
  SystemMonitorUtility::stopMeasurement(t_start, stat);
}

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(NetMonitor)
