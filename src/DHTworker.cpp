/**
 * Copyright (c) 2019-2021 polistern
 */

#include <mutex>
#include <thread>

#include "BoteContext.h"
#include "DHTworker.h"
#include "Packet.h"

namespace pbote {
namespace kademlia {

DHTworker DHT_worker;

DHTworker::DHTworker()
    : started_(false),
      m_worker_thread_(nullptr),
      local_node_(nullptr) {
  local_node_ = std::make_shared<Node>(context.getLocalDestination()->ToBase64());
}

DHTworker::~DHTworker() {
  stop();
}

void DHTworker::start() {
  std::string loglevel;
  pbote::config::GetOption("loglevel", loglevel);

  if (!isStarted()) {
    if (!loadNodes())
      LogPrint(eLogWarning, "DHT: have no nodes for start");

    if (loglevel == "debug" && !m_nodes_.empty()) {
      LogPrint(eLogDebug, "DHT: nodes stats:");
      for (const auto &node: m_nodes_)
        LogPrint(eLogDebug, "DHT: ", node.second->GetIdentHash().ToBase32());
      LogPrint(eLogDebug, "DHT: nodes stats end");
    }

    LogPrint(eLogDebug, "DHT: load local packets");
    dht_storage_.update();

    started_ = true;
    m_worker_thread_ = new std::thread(std::bind(&DHTworker::run, this));
  }
}

void DHTworker::stop() {
  LogPrint(eLogWarning, "DHT: stopping");
  if (isStarted()) {
    started_ = false;
    if (m_worker_thread_) {
      m_worker_thread_->join();
      delete m_worker_thread_;
      m_worker_thread_ = nullptr;
    }
  }
  LogPrint(eLogWarning, "DHT: stopped");
}

bool DHTworker::addNode(const std::string &dest) {
  i2p::data::IdentityEx identity;
  if (identity.FromBase64(dest)) {
    return addNode(identity);
  } else {
    LogPrint(eLogDebug, "DHT: addNode: Can't create node from base64");
    return false;
  }
}

bool DHTworker::addNode(const uint8_t *buf, size_t len) {
  i2p::data::IdentityEx identity;
  if (identity.FromBuffer(buf, len))
    return addNode(identity);
  else {
    LogPrint(eLogWarning, "DHT: addNode: Can't create node from buffer");
    return false;
  }
}

bool DHTworker::addNode(const i2p::data::IdentityEx &identity) {
  if (findNode(identity.GetIdentHash())) {
    //LogPrint(eLogDebug, "DHT: addNode: Duplicated node");
    return false;
  }

  auto local_destination = context.getLocalDestination();
  if (*local_destination == identity) {
    LogPrint(eLogDebug, "DHT: addNode: skip local destination");
    return false;
  }

  auto node = std::make_shared<Node>();
  node->FromBase64(identity.ToBase64());
  std::unique_lock<std::mutex> l(m_nodes_mutex_);
  return m_nodes_.insert(std::pair<i2p::data::IdentHash, std::shared_ptr<Node>>(node->GetIdentHash(), node)).second;
}

std::shared_ptr<Node> DHTworker::findNode(const i2p::data::IdentHash &ident) const {
  std::unique_lock<std::mutex> l(m_nodes_mutex_);
  auto it = m_nodes_.find(ident);
  if (it != m_nodes_.end())
    return it->second;
  else
    return nullptr;
}

std::shared_ptr<Node> DHTworker::getClosestNode(const i2p::data::IdentHash &key, bool to_us) {
  return std::make_shared<Node>(getClosestNodes(key, 1, to_us)[0]);
}

std::vector<Node> DHTworker::getClosestNodes(i2p::data::IdentHash key, size_t num, bool to_us) {
  struct sortable_node {
    std::shared_ptr<const Node> node;
    i2p::data::XORMetric metric;
    bool operator< (const sortable_node &other) const { return metric < other.metric; };
  };

  std::set<sortable_node> sorted_nodes;
  //i2p::data::IdentHash destKey = CreateRoutingKey(key);
  i2p::data::XORMetric minMetric = {};

  if (to_us)
    minMetric = key ^ local_node_->GetIdentHash();
  else
    minMetric.SetMax();

  std::unique_lock<std::mutex> l(m_nodes_mutex_);
  for (const auto &it: m_nodes_) {
    if (!it.second->locked()) {
      /// The XOR result for two hashes will be the larger, the more they differ by byte.
      /// In this case, we are interested in the minimum difference (distance).
      i2p::data::XORMetric metric = key ^ it.second->GetIdentHash();

      // ToDo: print distance in human readable format
      //std::string minMetric_S(minMetric.metric, minMetric.metric + 32);
      std::string metric_S(metric.metric, metric.metric + 32);
      //LogPrint(eLogDebug, "DHT: getClosestNodes: Metric.metric_ll: ", ToHex(minMetric_S, false));
      LogPrint(eLogDebug, "DHT: getClosestNodes: metric: ", ToHex(metric_S, false));
      //long long metric_ll = (long long)metric.metric;
      //LogPrint(eLogDebug, "DHT: getClosestNodes: metric: ", (uint64_t)metric.metric,
      //         (uint64_t)(metric.metric + 8), (uint64_t)(metric.metric + 16), (uint64_t)(metric.metric + 24));
      //BIGNUM *metric_bn;
      //metric_bn = BN_bin2bn(metric.metric, 32, nullptr);
      //LogPrint(eLogDebug, "DHT: getClosestNodes: metric_bn: ", metric_bn);
      //BN_free(metric_bn);

      //if (to_us && minMetric < metric) {
      //  continue;
      //}

      if (metric < minMetric) {
        minMetric = metric;
      }

      if (sorted_nodes.size() < num) {
        sorted_nodes.insert({it.second, metric});
      } else if (metric < sorted_nodes.rbegin()->metric) {
        sorted_nodes.insert({it.second, metric});
        sorted_nodes.erase(std::prev(sorted_nodes.end()));
      }
    }
  }

  std::vector<Node> result;
  size_t i = 0;
  for (const auto &it: sorted_nodes) {
    if (i < num) {
      result.push_back(*it.node);
      i++;
    } else
      break;
  }

  return result;
}

std::vector<Node> DHTworker::getAllNodes() {
  std::vector<Node> result;
  for (const auto &node: m_nodes_)
    result.push_back(*node.second);
  return result;
}

std::vector<Node> DHTworker::getUnlockedNodes() {
  std::vector<Node> res;
  size_t i = 0;
  std::unique_lock<std::mutex> l(m_nodes_mutex_);
  for (const auto &it: m_nodes_) {
    if (!it.second->locked()) {
      res.push_back(*it.second);
      i++;
    }
  }
  return res;
}

std::vector<std::shared_ptr<pbote::CommunicationPacket>> DHTworker::findOne(i2p::data::Tag<32> hash, uint8_t type) {
  return find(hash, type, false);
}

std::vector<std::shared_ptr<pbote::CommunicationPacket>> DHTworker::findAll(i2p::data::Tag<32> hash, uint8_t type) {
  return find(hash, type, true);
}

std::vector<std::shared_ptr<pbote::CommunicationPacket>> DHTworker::find(i2p::data::Tag<32> key,
                                                                         uint8_t type,
                                                                         bool exhaustive) {
  auto batch = std::make_shared<pbote::PacketBatch<pbote::CommunicationPacket>>();
  batch->owner = "DHT::find";
  LogPrint(eLogDebug, "DHT: find: Get closest nodes");

  std::vector<Node> closestNodes = closestNodesLookupTask(key);

  // ToDo: add find locally

  LogPrint(eLogDebug, "DHT: find: closest nodes count: ", closestNodes.size());
  if (closestNodes.size() < MIN_CLOSEST_NODES) {
    LogPrint(eLogInfo, "DHT: find: not enough nodes for find, try to use usual nodes");
    for (const auto &node: m_nodes_)
      closestNodes.push_back(*node.second);
    LogPrint(eLogDebug, "DHT: find: usual nodes count: ", closestNodes.size());
    if (closestNodes.size() < MIN_CLOSEST_NODES) {
      LogPrint(eLogWarning, "DHT: find: not enough nodes for find");
      return {};
    }
  }

  LogPrint(eLogDebug, "DHT: find: Start to find type: ", type, ", hash: ", key.ToBase64());
  for (const auto &node: closestNodes) {
    auto packet = retrieveRequestPacket(type, key);
    PacketForQueue q_packet(node.ToBase64(), packet.toByte().data(), packet.toByte().size());

    std::vector<uint8_t> v_cid(std::begin(packet.cid), std::end(packet.cid));
    batch->addPacket(v_cid, q_packet);
  }
  LogPrint(eLogDebug, "DHT: find: batch.size: ", batch->packetCount());
  context.send(batch);

  if (exhaustive)
    batch->waitLast(RESPONSE_TIMEOUT);
  else
    batch->waitFist(RESPONSE_TIMEOUT);

  int counter = 0;
  while (batch->responseCount() < 1 && counter < 5) {
    LogPrint(eLogWarning, "DHT: find: have no responses, try to resend batch, try #", counter);
    context.removeBatch(batch);
    context.send(batch);

    if (exhaustive)
      batch->waitLast(RESPONSE_TIMEOUT);
    else
      batch->waitFist(RESPONSE_TIMEOUT);
    counter++;
  }
  LogPrint(eLogDebug, "DHT: find: ", batch->responseCount(), " responses for ", key.ToBase64(), ", type: ", type);
  context.removeBatch(batch);

  return batch->getResponses();
}

std::vector<std::string> DHTworker::store(i2p::data::Tag<32> hash, uint8_t type, pbote::StoreRequestPacket packet) {
  auto batch = std::make_shared<pbote::PacketBatch<pbote::CommunicationPacket>>();
  batch->owner = "DHTworker::store";
  LogPrint(eLogDebug, "DHT: store: Get closest nodes");

  std::vector<Node> closestNodes = closestNodesLookupTask(hash);

  // ToDo: add find locally

  LogPrint(eLogDebug, "DHT: store: closest nodes count: ", closestNodes.size());
  if (closestNodes.size() < MIN_CLOSEST_NODES) {
    LogPrint(eLogInfo, "DHT: store: not enough nodes for store, try to use usual nodes");
    for (const auto &node: m_nodes_)
      closestNodes.push_back(*node.second);
    LogPrint(eLogDebug, "DHT: store: usual nodes count: ", closestNodes.size());
    if (closestNodes.size() < MIN_CLOSEST_NODES) {
      LogPrint(eLogWarning, "DHT: store: not enough nodes for store");
      return {};
    }
  }

  LogPrint(eLogDebug, "DHT: store: Start to store type: ", type, ", hash: ", hash.ToBase64());
  for (const auto &node: closestNodes) {
    context.random_cid(packet.cid, 32);
    auto packet_bytes = packet.toByte();
    PacketForQueue q_packet(node.ToBase64(), packet_bytes.data(), packet_bytes.size());

    std::vector<uint8_t> v_cid(std::begin(packet.cid), std::end(packet.cid));
    batch->addPacket(v_cid, q_packet);
  }
  LogPrint(eLogDebug, "DHT: store: batch.size: ", batch->packetCount());
  context.send(batch);

  batch->waitLast(RESPONSE_TIMEOUT);

  int counter = 0;
  while (batch->responseCount() < 1 && counter < 5) {
    LogPrint(eLogWarning, "DHT: store: have no responses, try to resend batch, try #", counter);
    context.removeBatch(batch);
    context.send(batch);
    // ToDo: remove answered nodes from batch
    batch->waitLast(RESPONSE_TIMEOUT);
    counter++;
  }
  LogPrint(eLogDebug, "DHT: store: ", batch->responseCount(), " responses for ", hash.ToBase64(), ", type: ", type);
  context.removeBatch(batch);

  std::vector<std::string> res;

  auto responses = batch->getResponses();

  res.reserve(responses.size());
  for (const auto &response: responses)
    res.push_back(response->from);

  return res;
}

std::vector<std::string> DHTworker::deleteEmail(i2p::data::Tag<32> hash, uint8_t type,
                                                pbote::EmailDeleteRequestPacket packet) {
  auto batch = std::make_shared<pbote::PacketBatch<pbote::CommunicationPacket>>();
  batch->owner = "DHTworker::deleteEmail";
  LogPrint(eLogDebug, "DHT: deleteEmail: Get closest nodes");

  std::vector<Node> closestNodes = closestNodesLookupTask(hash);

  // ToDo: add remove locally

  LogPrint(eLogDebug, "DHT: deleteEmail: closest nodes count: ", closestNodes.size());
  if (closestNodes.size() < MIN_CLOSEST_NODES) {
    LogPrint(eLogInfo, "DHT: deleteEmail: not enough nodes for delete request, try to use usual nodes");
    for (const auto &node: m_nodes_)
      closestNodes.push_back(*node.second);
    LogPrint(eLogDebug, "DHT: deleteEmail: usual nodes count: ", closestNodes.size());
    if (closestNodes.size() < MIN_CLOSEST_NODES) {
      LogPrint(eLogWarning, "DHT: deleteEmail: not enough nodes for delete request");
      return {};
    }
  }

  LogPrint(eLogDebug, "DHT: deleteEmail: Start to delete type: ", type, ", hash: ", hash.ToBase64());
  for (const auto &node: closestNodes) {
    context.random_cid(packet.cid, 32);
    auto packet_bytes = packet.toByte();
    PacketForQueue q_packet(node.ToBase64(), packet_bytes.data(), packet_bytes.size());

    std::vector<uint8_t> v_cid(std::begin(packet.cid), std::end(packet.cid));
    batch->addPacket(v_cid, q_packet);
  }
  LogPrint(eLogDebug, "DHT: deleteEmail: batch.size: ", batch->packetCount());
  context.send(batch);

  batch->waitLast(RESPONSE_TIMEOUT);

  int counter = 0;
  while (batch->responseCount() < 1 && counter < 5) {
    LogPrint(eLogWarning, "DHT: deleteEmail: have no responses, try to resend batch, try #", counter);
    context.removeBatch(batch);
    context.send(batch);
    // ToDo: remove answered nodes from batch
    batch->waitLast(RESPONSE_TIMEOUT);
    counter++;
  }
  LogPrint(eLogDebug, "DHT: deleteEmail: ", batch->responseCount(), " responses for ", hash.ToBase64(), ", type: ", type);
  context.removeBatch(batch);

  std::vector<std::string> res;

  auto responses = batch->getResponses();

  res.reserve(responses.size());
  for (const auto &response: responses)
    res.push_back(response->from);

  return res;
}

std::vector<std::string> DHTworker::deleteIndexEntry(i2p::data::Tag<32> index_dht_key,
                                                     i2p::data::Tag<32> email_dht_key,
                                                     i2p::data::Tag<32> del_auth) {
  auto batch = std::make_shared<pbote::PacketBatch<pbote::CommunicationPacket>>();
  batch->owner = "DHTworker::deleteIndexEntry";
  LogPrint(eLogDebug, "DHT: deleteIndexEntry: Get closest nodes");

  std::vector<Node> closestNodes = closestNodesLookupTask(index_dht_key);

  // ToDo: add remove locally

  LogPrint(eLogDebug, "DHT: deleteIndexEntry: closest nodes count: ", closestNodes.size());
  if (closestNodes.size() < MIN_CLOSEST_NODES) {
    LogPrint(eLogInfo, "DHT: deleteIndexEntry: not enough nodes for delete request, try to use usual nodes");
    for (const auto &node: m_nodes_)
      closestNodes.push_back(*node.second);
    LogPrint(eLogDebug, "DHT: deleteIndexEntry: usual nodes count: ", closestNodes.size());
    if (closestNodes.size() < MIN_CLOSEST_NODES) {
      LogPrint(eLogWarning, "DHT: deleteIndexEntry: not enough nodes for delete request");
      return {};
    }
  }

  LogPrint(eLogDebug, "DHT: deleteIndexEntry: Start to delete key: ", email_dht_key.ToBase64(), ", hash: ", del_auth.ToBase64());
  for (const auto &node: closestNodes) {
    pbote::IndexDeleteRequestPacket packet;
    context.random_cid(packet.cid, 32);

    memcpy(packet.dht_key, index_dht_key.data(), 32);
    packet.count = 1;
    pbote::IndexDeleteRequestPacket::item item{};
    memcpy(item.key, email_dht_key.data(), 32);
    memcpy(item.da, del_auth.data(), 32);

    packet.data.push_back(item);

    auto packet_bytes = packet.toByte();
    PacketForQueue q_packet(node.ToBase64(), packet_bytes.data(), packet_bytes.size());

    std::vector<uint8_t> v_cid(std::begin(packet.cid), std::end(packet.cid));
    batch->addPacket(v_cid, q_packet);
  }
  LogPrint(eLogDebug, "DHT: deleteIndexEntry: batch.size: ", batch->packetCount());
  context.send(batch);

  batch->waitLast(RESPONSE_TIMEOUT);

  int counter = 0;
  while (batch->responseCount() < 1 && counter < 5) {
    LogPrint(eLogWarning, "DHT: deleteIndexEntry: have no responses, try to resend batch, try #", counter);
    context.removeBatch(batch);
    context.send(batch);
    // ToDo: remove answered nodes from batch
    batch->waitLast(RESPONSE_TIMEOUT);
    counter++;
  }
  LogPrint(eLogDebug, "DHT: deleteIndexEntry: ", batch->responseCount(), " responses for ", email_dht_key.ToBase64());
  context.removeBatch(batch);

  std::vector<std::string> res;

  auto responses = batch->getResponses();

  res.reserve(responses.size());
  for (const auto &response: responses)
    res.push_back(response->from);

  return res;
}

std::vector<Node> DHTworker::closestNodesLookupTask(i2p::data::Tag<32> key) {
  unsigned long current_time, exec_duration;
  auto batch = std::make_shared<pbote::PacketBatch<pbote::CommunicationPacket>>();
  batch->owner = "DHT::closestNodesLookupTask";
  std::vector<Node> closestNodes;
  std::vector<std::shared_ptr<pbote::CommunicationPacket>> responses;

  /// Set start time
  auto task_start_time = std::chrono::system_clock::now().time_since_epoch().count();
  //auto not_queried_nodes = getUnlockedNodes();
  auto req_nodes = getAllNodes();
  for (const auto &node: req_nodes) {
    /// Create find closest peers packet
    auto packet = findClosePeersPacket(key);
    auto bytes = packet.toByte();
    PacketForQueue q_packet(node.ToBase64(), bytes.data(), bytes.size());
    std::vector<uint8_t> v_cid(std::begin(packet.cid), std::end(packet.cid));
    /// Copy packet to pending task for check timeout later
    active_requests.insert(std::pair<std::vector<uint8_t>, std::shared_ptr<Node>>(v_cid, std::make_shared<Node>(node)));
    batch->addPacket(v_cid, q_packet);
  }

  current_time = std::chrono::system_clock::now().time_since_epoch().count();
  exec_duration = (current_time - task_start_time) / 1000000000;

  /// While unanswered requests less than Kademlia CONSTANT_ALPHA and we have non queried nodes
  //while (active_requests.size() < CONSTANT_ALPHA && !not_queried_nodes.empty()) {
  while (!active_requests.empty() && exec_duration < CLOSEST_NODES_LOOKUP_TIMEOUT) {
    LogPrint(eLogDebug, "DHT: closestNodesLookupTask: batch.size: ", batch->packetCount());
    context.send(batch);
    batch->waitLast(RESPONSE_TIMEOUT);
    responses = batch->getResponses();
    if (!responses.empty()) {
      LogPrint(eLogDebug, "DHT: closestNodesLookupTask: ", responses.size(), " responses for ", key.ToBase64());
      for (const auto &response: responses) {
        std::vector<uint8_t> v_cid(std::begin(response->cid), std::end(response->cid));
        // check if we sent requests with this CID
        // ToDo: decrease if have no response
        if (active_requests.find(v_cid) != active_requests.end()) {
          // mark that the node sent response
          auto peer = active_requests[v_cid];
          peer->gotResponse();
          // remove node from active requests
          active_requests.erase(v_cid);
        }
      }
      // ToDo: remove in release
      if (responses.size() >= MIN_CLOSEST_NODES)
        break;
    } else {
      LogPrint(eLogWarning, "DHT: closestNodesLookupTask: have no responses, try to resend batch");
      context.removeBatch(batch);
    }
    current_time = std::chrono::system_clock::now().time_since_epoch().count();
    exec_duration = (current_time - task_start_time) / 1000000000;
  }

  // if we have at least one response
  for (const auto &response: responses) {
    if (response->type != type::CommN) {
      // ToDo: looks like in case if we got request to ourself, for now we just skip it
      LogPrint(eLogWarning, "DHT: closestNodesLookupTask: got non-response packet in batch, type: ",
               response->type, ", ver: ", unsigned(response->ver));
      continue;
    }

    size_t offset = 0;
    uint8_t status;
    uint16_t dataLen;

    std::memcpy(&status, response->payload.data(), 1);
    offset += 1;
    std::memcpy(&dataLen, response->payload.data() + offset, 2);
    dataLen = ntohs(dataLen);
    offset += 2;

    if (status != StatusCode::OK) {
      LogPrint(eLogWarning, "DHT: closestNodesLookupTask: status: ", statusToString(status));
      continue;
    }

    if (dataLen < 4) {
      LogPrint(eLogWarning, "DHT: closestNodesLookupTask: packet without payload, skip parsing");
      continue;
    }

    uint8_t data[dataLen];
    std::memcpy(&data, response->payload.data() + offset, dataLen);

    LogPrint(eLogDebug, "DHT: closestNodesLookupTask: type: ", response->type, ", ver: ", unsigned(response->ver));
    std::vector<Node> peers_list;
    if (unsigned(data[1]) == 4 && (data[0] == (uint8_t) 'L' || data[0] == (uint8_t) 'P')) {
      peers_list = receivePeerListV4(data, dataLen);
    }
    if (unsigned(data[1]) == 5 && (data[0] == (uint8_t) 'L' || data[0] == (uint8_t) 'P')) {
      peers_list = receivePeerListV5(data, dataLen);
    }

    if (!peers_list.empty()) {
      closestNodes.insert(closestNodes.end(), peers_list.begin(), peers_list.end());
    }
  }

  /// If there are no more requests to send, and no more responses to wait for, we're finished
  context.removeBatch(batch);

  for (const auto &node: closestNodes) addNode(node);

  current_time = std::chrono::system_clock::now().time_since_epoch().count();
  exec_duration = (current_time - task_start_time) / 1000000000;
  if (exec_duration < CLOSEST_NODES_LOOKUP_TIMEOUT) {
    //LogPrint(eLogDebug, "DHT: closestNodesLookupTask: wait for ", CLOSEST_NODES_LOOKUP_TIMEOUT - exec_duration, " sec.");
    //std::this_thread::sleep_for(std::chrono::seconds(CLOSEST_NODES_LOOKUP_TIMEOUT - exec_duration));
    LogPrint(eLogDebug, "DHT: closestNodesLookupTask: finished");
  } else {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    LogPrint(eLogDebug, "DHT: closestNodesLookupTask: finished");
  }
  return closestNodes;
}

std::vector<Node> DHTworker::receivePeerListV4(const uint8_t *buf, size_t len) {
  size_t offset = 0;
  uint8_t type, ver;
  uint16_t nodes_count;

  std::memcpy(&type, buf, 1);
  offset += 1;
  std::memcpy(&ver, buf + offset, 1);
  offset += 1;
  std::memcpy(&nodes_count, buf + offset, 2);
  offset += 2;
  nodes_count = ntohs(nodes_count);

  if ((type == (uint8_t) 'L' || type == (uint8_t) 'P') && ver == (uint8_t) 4) {
    std::vector<Node> closestNodes;
    size_t nodes_added = 0, nodes_dup = 0;
    for (size_t i = 0; i < nodes_count; i++) {
      if (offset == len) {
        LogPrint(eLogWarning, "DHT: receivePeerListV4: end of packet!");
        break;
      }
      if (offset + 384 > len) {
        LogPrint(eLogWarning, "DHT: receivePeerListV4: incomplete packet!");
        break;
      }

      uint8_t fullKey[387];
      memcpy(fullKey, buf + offset, 384);
      offset += 384;

      i2p::data::IdentityEx node;

      /// This is an workaround, but the current version of the
      /// protocol does not allow the correct key type to be determined
      fullKey[384] = 0;
      fullKey[385] = 0;
      fullKey[386] = 0;

      size_t res = node.FromBuffer(fullKey, 387);
      if (res > 0) {
        if (addNode(fullKey, 387)) {
          closestNodes.emplace_back(node.ToBase64());
          nodes_added++;
        } else {
          nodes_dup++;
        }
      } else
        LogPrint(eLogWarning, "DHT: receivePeerListV4: fail to add node");
    }
    LogPrint(eLogDebug, "DHT: receivePeerListV4: nodes: ", nodes_count, ", added: ", nodes_added,
             ", dup: ", nodes_dup);
    return closestNodes;
  } else {
    LogPrint(eLogWarning, "DHT: receivePeerListV4: unknown packet, type: ", type, ", ver: ", unsigned(ver));
    return {};
  }
}

std::vector<Node> DHTworker::receivePeerListV5(const uint8_t *buf, size_t len) {
  size_t offset = 0;
  uint8_t type, ver;
  uint16_t nump;

  std::memcpy(&type, buf, 1);
  offset += 1;
  std::memcpy(&ver, buf + offset, 1);
  offset += 1;
  std::memcpy(&nump, buf + offset, 2);
  offset += 2;
  nump = ntohs(nump);

  if ((type == (uint8_t) 'L' || type == (uint8_t) 'P') && ver == (uint8_t) 5) {
    std::vector<Node> closestNodes;
    size_t nodes_added = 0, nodes_dup = 0;
    for (size_t i = 0; i < nump; i++) {
      if (offset == len) {
        LogPrint(eLogWarning, "DHT: receivePeerListV5: end of packet");
        break;
      }
      if (offset + 384 > len) {
        LogPrint(eLogWarning, "DHT: receivePeerListV5: incomplete packet");
        break;
      }

      i2p::data::IdentityEx identity;

      size_t key_len = identity.FromBuffer(buf + offset, len - offset);
      offset += key_len;
      if (key_len > 0) {
        if (addNode(identity)) {
          //LogPrint(eLogDebug, "DHT: receivePeerListV5: add node sign: ", sign_type, ", res: ", res);
          nodes_added++;
          closestNodes.emplace_back(identity.ToBase64());
        } else {
          //LogPrint(eLogWarning, "DHT: receivePeerListV5: fail to add node with sign: ", sign_type);
          nodes_dup++;
        }
      } else
        LogPrint(eLogWarning, "DHT: receivePeerListV5: fail to add node");
    }
    LogPrint(eLogDebug,
             "DHT: receivePeerListV5: nodes: ", nump, ", added: ", nodes_added, ", dup: ", nodes_dup);
    return closestNodes;
  } else {
    LogPrint(eLogWarning, "DHT: receivePeerListV5: unknown packet, type: ", type, ", ver: ", unsigned(ver));
    return {};
  }
}

void DHTworker::receiveRetrieveRequest(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: add requester to nodes list");
  }

  uint16_t offset = 0;
  uint8_t dataType;
  uint8_t key[32];

  std::memcpy(&dataType, packet->payload.data(), 1);
  offset += 1;
  std::memcpy(&key, packet->payload.data() + offset, 32); //offset += 32;

  pbote::ResponsePacket response;
  memcpy(response.cid, packet->cid, 32);

  if (dataType == (uint8_t) 'I' || dataType == (uint8_t) 'E' || dataType == (uint8_t) 'C') {
    i2p::data::Tag<32> hash(key);
    LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: request for type: ", dataType, ", key: ", hash.ToBase64());

    /// Try to find packet in storage
    std::vector<uint8_t> data;
    switch (dataType) {
      case ((uint8_t) 'I'): data = dht_storage_.getIndex(hash);
        break;
      case ((uint8_t) 'E'): data = dht_storage_.getEmail(hash);
        break;
      case ((uint8_t) 'C'): data = dht_storage_.getContact(hash);
        break;
      default:break;
    }

    if (data.empty()) {
      LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: can't find type: ", dataType, ", key: ", hash.ToBase64());
      response.status = pbote::StatusCode::NO_DATA_FOUND;
      response.length = 0;
    } else {
      LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: found data type: ", dataType, ", key: ", hash.ToBase64());
      response.status = pbote::StatusCode::OK;
      response.length = data.size();
      response.data = data;
    }
  } else {
    // In case if we can't parse
    LogPrint(eLogDebug, "DHT: receiveRetrieveRequest: unknown packet type: ", dataType);
    response.status = pbote::StatusCode::INVALID_PACKET;
    response.length = 0;
  }

  PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
  context.send(q_packet);
}

void DHTworker::receiveDeletionQuery(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveDeletionQuery: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveDeletionQuery: add requester to nodes list");
  }

  uint8_t key[32];

  pbote::ResponsePacket response;
  memcpy(response.cid, packet->cid, 32);

  if (packet->payload.size() == 32) {
    std::memcpy(&key, packet->payload.data(), 32);
    i2p::data::Tag<32> t_key(key);

    LogPrint(eLogDebug, "DHT: receiveDeletionQuery: got request for key: ", t_key.ToBase64());

    auto data = dht_storage_.getEmail(key);
    if (data.empty()) {
      LogPrint(eLogDebug, "DHT: receiveDeletionQuery: key not found: ", t_key.ToBase64());
      response.status = pbote::StatusCode::NO_DATA_FOUND;
      response.length = 0;
    } else {
      LogPrint(eLogDebug, "DHT: receiveDeletionQuery: found key: ", t_key.ToBase64());

      // ToDo: delete local packet

      response.status = pbote::StatusCode::OK;
      response.length = 0;
    }
  } else {
    // In case if can't parse
    LogPrint(eLogDebug, "DHT: receiveDeletionQuery: Packet is too short");
    response.status = pbote::StatusCode::INVALID_PACKET;
    response.length = 0;
  }

  PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
  context.send(q_packet);
}

void DHTworker::receiveStoreRequest(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveStoreRequest: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveStoreRequest: add requester to nodes list");
  }

  uint16_t offset = 0;
  StoreRequestPacket new_packet;

  std::memcpy(&new_packet.cid, packet->cid, 32);

  std::memcpy(&new_packet.hc_length, packet->payload.data(), 2);
  new_packet.hc_length = ntohs(new_packet.hc_length);
  offset += 2;
  LogPrint(eLogDebug, "DHT: receiveStoreRequest: hc_length: ", new_packet.hc_length);

  uint8_t hashCash[new_packet.hc_length];
  std::memcpy(&hashCash, packet->payload.data() + offset, new_packet.hc_length);
  offset += new_packet.hc_length;

  std::memcpy(&new_packet.length, packet->payload.data() + offset, 2);
  new_packet.length = ntohs(new_packet.length);
  offset += 2;
  LogPrint(eLogDebug, "DHT: receiveStoreRequest: length: ", new_packet.length);

  //uint8_t data[new_packet.length];
  //std::memcpy(&data, packet->payload.data() + offset, new_packet.length);
  //new_packet.data = std::vector<uint8_t>(data, data + new_packet.length);
  new_packet.data = std::vector<uint8_t>(packet->payload.begin() + offset,
                                         packet->payload.begin() + offset + new_packet.length);

  LogPrint(eLogDebug, "DHT: receiveStoreRequest: got request for type: ", new_packet.data[0],
           ", ver: ", unsigned(new_packet.data[1]));

  pbote::ResponsePacket response;
  memcpy(response.cid, packet->cid, 32);

  if ((new_packet.data[0] == (uint8_t) 'I' || new_packet.data[0] == (uint8_t) 'E' || new_packet.data[0] == (uint8_t) 'C') && new_packet.data[1] == 4) {
    // ToDo: Check available space
    //response.status = pbote::StatusCode::NO_DISK_SPACE;
    //response.length = 0;

    // ToDo: Check if not enough HashCash provided
    //response.status = pbote::StatusCode::INSUFFICIENT_HASHCASH;
    //response.length = 0;

    // ToDo: Check HashCash
    //response.status = pbote::StatusCode::INVALID_HASHCASH;
    //response.length = 0;

    if (dht_storage_.safe(new_packet.data)) {
      LogPrint(eLogDebug, "DHT: receiveStoreRequest: packet saved");
      response.status = pbote::StatusCode::OK;
      response.length = 0;
    } else {
      LogPrint(eLogWarning, "DHT: receiveStoreRequest: got error while try to save packet");
      response.status = pbote::StatusCode::GENERAL_ERROR;
      response.length = 0;
    }
  } else {
    LogPrint(eLogWarning, "DHT: receiveStoreRequest: unsupported packet, type: ", new_packet.data[0],
             ", ver: ", unsigned(new_packet.data[1]));
    response.status = pbote::StatusCode::INVALID_PACKET;
    response.length = 0;
  }

  PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
  context.send(q_packet);
}

void DHTworker::receiveEmailPacketDeleteRequest(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: add requester to nodes list");
  }

  uint16_t offset = 0;
  uint8_t key[32]{};
  uint8_t delAuth[32]{};
  uint8_t delHash[32]{};

  std::memcpy(&key, packet->payload.data(), 32);
  offset += 32;
  std::memcpy(&delAuth, packet->payload.data() + offset, 32); //offset += 32;

  i2p::data::Tag<32> t_key(key);
  LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: got request for key: ", t_key.ToBase64());

  pbote::ResponsePacket response;
  memcpy(response.cid, packet->cid, 32);

  // Get hash of Delete Auth
  SHA256(delAuth, 32, delHash);
  auto email_packet = dht_storage_.getEmail(t_key);

  // ToDo: re-think
  if (email_packet.empty()) {
    LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: key not found: ", t_key.ToBase64());
    response.status = pbote::StatusCode::NO_DATA_FOUND;
    response.length = 0;
  } else {
    LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: found key: ", t_key.ToBase64());

    // Get Delete Auth hash from email packet
    std::vector<uint8_t> email_delete_hash;
    if ( email_packet.size() < 70) {
      LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: local packet is too short: ", t_key.ToBase64());
      response.status = pbote::StatusCode::GENERAL_ERROR;
      response.length = 0;
    } else {
      email_delete_hash.insert(email_delete_hash.end(), email_packet.begin() + 38, email_packet.begin() + 70);

      // Compare hashes
      if (memcmp(delHash, email_delete_hash.data(), 32) != 0) {
        LogPrint(eLogWarning, "DHT: receiveEmailPacketDeleteRequest: delete auth hashes mismatch");
        response.status = pbote::StatusCode::INVALID_PACKET;
        response.length = 0;
      } else {
        LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: delete auth hashes match");

        if (dht_storage_.deleteEmail(t_key)) {
          LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: packet successfully removed");
          response.status = pbote::StatusCode::OK;
          response.length = 0;
        } else {
          LogPrint(eLogDebug, "DHT: receiveEmailPacketDeleteRequest: Can't remove packet");
          response.status = pbote::StatusCode::GENERAL_ERROR;
          response.length = 0;
        }
      }
    }
  }

  PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
  context.send(q_packet);
}

void DHTworker::receiveIndexPacketDeleteRequest(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: add requester to nodes list");
  }

  uint16_t offset = 0;
  uint8_t dh[32];
  uint8_t num;

  std::memcpy(&dh, packet->payload.data(), 32);
  offset += 32;
  std::memcpy(&num, packet->payload.data() + offset, 1);
  offset += 1;

  pbote::ResponsePacket response;
  memcpy(response.cid, packet->cid, 32);
  response.length = 0;

  i2p::data::Tag<32> t_key(dh);
  LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: got request for key: ", t_key.ToBase64());
  auto data = dht_storage_.getIndex(t_key);
  if (data.empty()) {
    LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: key not found: ", t_key.ToBase64());
    response.status = pbote::StatusCode::NO_DATA_FOUND;
  } else {
    LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: found key: ", t_key.ToBase64());

    pbote::IndexPacket index_packet;
    bool parsed = index_packet.fromBuffer(data, true);

    if (parsed) {
      bool equals = false;
      for (uint8_t i = 0; i < num; i++) {
        uint8_t dht[32];
        uint8_t delAuth[32];
        uint8_t delHash[32];

        std::memcpy(&dht, packet->payload.data() + offset, 32);
        offset += 32;
        std::memcpy(&delAuth, packet->payload.data() + offset, 32);
        offset += 32;

        // Get hash of Delete Auth
        SHA256(delAuth, 32, delHash);

        // Compare hashes
        for (uint8_t k = 0; k < (uint8_t)index_packet.data.size(); k++) {
          if (memcmp(delHash, index_packet.data[k].dv, 32) == 0) {
            equals = true;
            index_packet.data.erase(index_packet.data.begin() + k);
            index_packet.nump = index_packet.data.size();
            break;
          }
        }

        // Check result of compare
        if (equals) {
          LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: delete auth hashes match");
        } else {
          LogPrint(eLogWarning, "DHT: receiveIndexPacketDeleteRequest: delete auth hashes mismatch");
          response.status = pbote::StatusCode::INVALID_PACKET;
        }
      }
      if (equals) {
        // Delete "old" and write "new" packet, if not empty
        bool deleted = dht_storage_.deleteIndex(t_key);
        bool saved = false;

        if (!index_packet.data.empty())
          saved = dht_storage_.safe(index_packet.toByte());

        if (deleted && saved) {
          LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: packet replaced");
          response.status = pbote::StatusCode::OK;
        } else if (saved) {
          LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: new packet saved");
          response.status = pbote::StatusCode::OK;
        } else if (index_packet.data.empty() && deleted) {
          LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: delete empty packet");
          response.status = pbote::StatusCode::OK;
        } else {
          LogPrint(eLogError, "DHT: receiveIndexPacketDeleteRequest: Can't save new packet");
          response.status = pbote::StatusCode::GENERAL_ERROR;
        }
      }
    } else {
      LogPrint(eLogDebug, "DHT: receiveIndexPacketDeleteRequest: can't parse local packet for key: ", t_key.ToBase64());
      response.status = pbote::StatusCode::GENERAL_ERROR;
    }
  }

  PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
  context.send(q_packet);
}

void DHTworker::receiveFindClosePeers(const std::shared_ptr<pbote::CommunicationPacket> &packet) {
  LogPrint(eLogDebug, "DHT: receiveFindClosePeers: request from: ", packet->from);

  if (addNode(packet->from)) {
    LogPrint(eLogDebug, "DHT: receiveFindClosePeers: add requester to nodes list");
  }

  uint8_t key[32];
  std::memcpy(&key, packet->payload.data(), 32);
  i2p::data::Tag<32> t_key(key);

  LogPrint(eLogDebug, "DHT: receiveFindClosePeers: got request for key: ", t_key.ToBase64());


  // ToDo: just for tests
  //auto closest_nodes = getAllNodes();

  auto closest_nodes = getClosestNodes(t_key, KADEMLIA_CONSTANT_K, false);
  for (const auto& test : closest_nodes) {
    LogPrint(eLogDebug, "DHT: receiveFindClosePeers: node: ", test.GetIdentHash().ToBase32());
  }

  if (closest_nodes.empty()) {
    LogPrint(eLogDebug, "DHT: receiveFindClosePeers: Can't find closest nodes");

    pbote::ResponsePacket response;
    memcpy(response.cid, packet->cid, 32);
    response.status = pbote::StatusCode::GENERAL_ERROR;
    response.length = 0;

    PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
    context.send(q_packet);
  } else {
    LogPrint(eLogDebug,"DHT: receiveFindClosePeers: got ", closest_nodes.size(), " nodes closest to key: ",
             t_key.ToBase64());
    pbote::ResponsePacket response;
    memcpy(response.cid, packet->cid, 32);
    response.status = pbote::StatusCode::OK;

    if (packet->ver == 4) {
      LogPrint(eLogDebug, "DHT: receiveFindClosePeers: prepare PeerListPacketV4");
      pbote::PeerListPacketV4 peer_list;
      peer_list.count = closest_nodes.size();

      for (const auto &node: closest_nodes) {
        size_t ilen = node.GetFullLen();
        std::vector<uint8_t> buf(ilen);
        node.ToBuffer(buf.data(), ilen);
        peer_list.data.insert(peer_list.data.end(), buf.begin(), buf.end());
      }
      response.data = peer_list.toByte();
    }

    if (packet->ver == 5) {
      LogPrint(eLogDebug, "DHT: receiveFindClosePeers: prepare PeerListPacketV5");
      pbote::PeerListPacketV5 peer_list;
      peer_list.count = closest_nodes.size();

      for (const auto &node: closest_nodes) {
        size_t ilen = node.GetFullLen();
        std::vector<uint8_t> buf(ilen);
        node.ToBuffer(buf.data(), ilen);
        peer_list.data.insert(peer_list.data.end(), buf.begin(), buf.end());
      }
      response.data = peer_list.toByte();
    }

    response.length = response.data.size();

    LogPrint(eLogDebug, "DHT: receiveFindClosePeers: send response with ", closest_nodes.size(), " node(s)");
    PacketForQueue q_packet(packet->from, response.toByte().data(), response.toByte().size());
    context.send(q_packet);
  }
}

void DHTworker::run() {
  size_t counter = 0;
  std::string loglevel;
  pbote::config::GetOption("loglevel", loglevel);

  while (started_) {
    counter++;

    writeNodes();
    dht_storage_.update();

    if (counter > 10 && loglevel == "debug" && !m_nodes_.empty()) {
      LogPrint(eLogDebug, "DHT: nodes stats:");
      for (const auto &node: m_nodes_)
        LogPrint(eLogDebug, "DHT: ", node.second->ToBase64());
      LogPrint(eLogDebug, "DHT: nodes stats end");
      counter = 0;
    }

    std::this_thread::sleep_for(std::chrono::seconds(60));
  }
}

std::vector<std::string> DHTworker::readNodes() {
  std::string nodes_file_path = pbote::fs::DataDirPath("nodes.txt");
  LogPrint(eLogInfo, "DHT: readNodes: read nodes from ", nodes_file_path);
  std::ifstream nodes_file(nodes_file_path);

  if (!nodes_file.is_open()) {
    LogPrint(eLogError, "DHT: readNodes: can't open file ", nodes_file_path);
    return {};
  }

  std::vector<std::string> nodes_list;

  for (std::string line; getline(nodes_file, line);) {
    if (!line.empty() && line[0] != ('\n') && line[0] != '#')
      nodes_list.push_back(line);
  }
  return nodes_list;
}

bool DHTworker::loadNodes() {
  std::vector<std::string> nodes_list = readNodes();
  std::vector<Node> nodes;

  for (const auto &node_str: nodes_list) {
    //LogPrint(eLogDebug, "DHT: loadNodes: node_str: ", node_str);
    auto node = new Node(node_str);
    nodes.push_back(*node);
  }

  if (!nodes.empty()) {
    size_t counter = 0, dup = 0;
    for (const auto &node: nodes) {
      LogPrint(eLogDebug, "DHT: loadNodes: node.ToBase64(): ", node.ToBase64());
      auto t_hash = node.GetIdentHash();
      bool result = m_nodes_.insert(std::pair<i2p::data::IdentHash, std::shared_ptr<Node>>(t_hash,
                                                                                           std::make_shared<Node>(node))).second;
      if (result)
        counter++;
      else
        dup++;
    }
    if (counter == 0)
      LogPrint(eLogInfo, "DHT: loadNodes: can't load nodes, try bootstrap");
    else {
      LogPrint(eLogInfo, "DHT: loadNodes: nodes loaded: ", counter, ", duplicated: ", dup);
      return true;
    }
  }

  // Only if we have no nodes in storage
  std::vector<std::string> bootstrap_addresses;
  pbote::config::GetOption("bootstrap.address", bootstrap_addresses);

  if (!bootstrap_addresses.empty()) {
    for (auto &bootstrap_address: bootstrap_addresses) {
      if (addNode(bootstrap_address)) {
        i2p::data::IdentityEx new_node;
        new_node.FromBase64(bootstrap_address);
        LogPrint(eLogDebug, "DHT: loadNodes: successfully add node: ", new_node.GetIdentHash().ToBase64());
      }
    }
    return true;
  } else
    return false;
}

void DHTworker::writeNodes() {
  LogPrint(eLogInfo, "DHT: writeNodes: save nodes to FS");
  std::string nodes_file_path = pbote::fs::DataDirPath("nodes.txt");
  std::ofstream nodes_file(nodes_file_path);

  if (!nodes_file.is_open()) {
    LogPrint(eLogError, "DHT: writeNodes: can't open file ", nodes_file_path);
    return;
  }

  nodes_file << "# Each line is one Base64-encoded I2P destination.\n";
  nodes_file << "# Do not edit this file while pbote is running as it will be overwritten.\n\n";
  std::unique_lock<std::mutex> l(m_nodes_mutex_);

  for (const auto &node: m_nodes_) {
    nodes_file << node.second->ToBase64();
    nodes_file << "\n";
  }

  nodes_file.close();
  LogPrint(eLogDebug, "DHT: writeNodes: nodes saved to FS");
}

pbote::FindClosePeersRequestPacket DHTworker::findClosePeersPacket(i2p::data::Tag<32> key) {
  /// don't reuse request packets because PacketBatch will not add the same one more than once
  pbote::FindClosePeersRequestPacket packet;
  /// Java will be answer wuth v4, c++ - with v5
  packet.ver = 5;
  context.random_cid(packet.cid, 32);
  memcpy(packet.key, key.data(), 32);

  return packet;
}

pbote::RetrieveRequestPacket DHTworker::retrieveRequestPacket(uint8_t data_type, i2p::data::Tag<32> key) {
  pbote::RetrieveRequestPacket packet;
  context.random_cid(packet.cid, 32);
  memcpy(packet.key, key.data(), 32);
  packet.data_type = data_type;
  return packet;
}

} // namespace kademlia
} // namespace pbote
