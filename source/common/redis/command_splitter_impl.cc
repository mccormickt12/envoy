#include "common/redis/command_splitter_impl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common/assert.h"
#include "common/redis/supported_commands.h"

#include "fmt/format.h"

namespace Envoy {
namespace Redis {
namespace CommandSplitter {

RespValuePtr Utility::makeError(const std::string& error) {
  RespValuePtr response(new RespValue());
  response->type(RespType::Error);
  response->asString() = error;
  return response;
}

void SplitRequestBase::onWrongNumberOfArguments(SplitCallbacks& callbacks,
                                                const RespValue& request) {
  callbacks.onResponse(Utility::makeError(
      fmt::format("wrong number of arguments for '{}' command", request.asArray()[0].asString())));
}

SingleServerRequest::~SingleServerRequest() { ASSERT(!handle_); }

void SingleServerRequest::onResponse(RespValuePtr&& response) {
  handle_ = nullptr;
  callbacks_.onResponse(std::move(response));
}

void SingleServerRequest::onFailure() {
  handle_ = nullptr;
  callbacks_.onResponse(Utility::makeError("upstream failure"));
}

void SingleServerRequest::cancel() {
  handle_->cancel();
  handle_ = nullptr;
}

SplitRequestPtr SimpleRequest::create(ConnPool::Instance& conn_pool,
                                      const RespValue& incoming_request,
                                      SplitCallbacks& callbacks) {
  std::unique_ptr<SimpleRequest> request_ptr{new SimpleRequest(callbacks)};

  request_ptr->handle_ = conn_pool.makeRequest(incoming_request.asArray()[1].asString(),
                                               incoming_request, *request_ptr);
  if (!request_ptr->handle_) {
    request_ptr->callbacks_.onResponse(Utility::makeError("no upstream host"));
    return nullptr;
  }

  return std::move(request_ptr);
}

SplitRequestPtr EvalRequest::create(ConnPool::Instance& conn_pool,
                                    const RespValue& incoming_request, SplitCallbacks& callbacks) {

  // EVAL looks like: EVAL script numkeys key [key ...] arg [arg ...]
  // Ensure there are at least three args to the command or it cannot be hashed.
  if (incoming_request.asArray().size() < 4) {
    onWrongNumberOfArguments(callbacks, incoming_request);
    return nullptr;
  }

  std::unique_ptr<EvalRequest> request_ptr{new EvalRequest(callbacks)};
  request_ptr->handle_ = conn_pool.makeRequest(incoming_request.asArray()[3].asString(),
                                               incoming_request, *request_ptr);
  if (!request_ptr->handle_) {
    request_ptr->callbacks_.onResponse(Utility::makeError("no upstream host"));
    return nullptr;
  }

  return std::move(request_ptr);
}

FragmentedRequest::~FragmentedRequest() {
#ifndef NDEBUG
  for (const PendingRequest& request : pending_requests_) {
    ASSERT(!request.handle_);
  }
#endif
}

void FragmentedRequest::cancel() {
  for (PendingRequest& request : pending_requests_) {
    if (request.handle_) {
      request.handle_->cancel();
      request.handle_ = nullptr;
    }
  }
}

void FragmentedRequest::onChildFailure(uint32_t index, std::vector<uint32_t> response_indexes) {
  onChildResponse(Utility::makeError("upstream failure"), index, response_indexes);
}

SplitRequestPtr MGETRequest::create(ConnPool::Instance& conn_pool,
                                    const RespValue& incoming_request, SplitCallbacks& callbacks) {
  // Generate a map of hosts to keys and their original index in the request.
  std::unordered_map<std::string, std::vector<std::pair<std::string, uint32_t>>> request_map;
  for (uint32_t i = 1; i < incoming_request.asArray().size(); i++) {
    const std::string& key = incoming_request.asArray()[i].asString();
    const std::string& host = conn_pool.getHost(key);
    
    const std::pair<std::string, uint32_t> key_and_index {key, i - 1}; 
    auto collapsed_request = request_map.find(host);
    if (collapsed_request != request_map.end()) {
      collapsed_request->second.push_back(key_and_index);
    } else {
      request_map[host] = {key_and_index};
    }
  }

  // Initialize empty request.
  std::unique_ptr<MGETRequest> request_ptr{new MGETRequest(callbacks)};
  request_ptr->num_pending_responses_ = request_map.size();
  request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

  // Initalize empty response.
  request_ptr->pending_response_.reset(new RespValue());
  request_ptr->pending_response_->type(RespType::Array);
  std::vector<RespValue> responses(incoming_request.asArray().size() - 1);
  request_ptr->pending_response_->asArray().swap(responses);
  
  // Each entry in the map represents a request. Create each request and pass the original index of
  // each key with it.
  RespValue mget; 
  mget.type(RespType::Array);

  uint32_t request_index{};
  for (const auto& request : request_map) {
    const std::vector<std::pair<std::string, uint32_t>>& key_index_pairs = request.second;

    std::vector<RespValue> collapsed_request(key_index_pairs.size() + 1);
    collapsed_request[0].type(RespType::BulkString);
    collapsed_request[0].asString() = "MGET";
    
    std::vector<uint32_t> response_indexes;
    response_indexes.reserve(key_index_pairs.size());
    for (uint32_t i = 0; i < key_index_pairs.size(); i++) {
      collapsed_request[i + 1].type(RespType::BulkString);
      collapsed_request[i + 1].asString() = key_index_pairs[i].first;
      response_indexes.push_back(key_index_pairs[i].second);
    }
    mget.asArray().swap(collapsed_request);

    request_ptr->pending_requests_.emplace_back(*request_ptr, request_index++, response_indexes);
    PendingRequest& pending_request = request_ptr->pending_requests_.back();

    pending_request.handle_ = conn_pool.makeRequest(mget.asArray()[1].asString(), mget, pending_request);
    if (!pending_request.handle_) {
      pending_request.onResponse(Utility::makeError("no upstream host"));
    }
  }

  return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
}

void MGETRequest::onChildResponse(RespValuePtr&& value, uint32_t index,
                                  std::vector<uint32_t> response_indexes) {
  pending_requests_[index].handle_ = nullptr;
  
  switch (value->type()) {
  case RespType::Integer:
  case RespType::Null:
  case RespType::SimpleString: {
    for (const uint32_t response_index : response_indexes) {
      pending_response_->asArray()[response_index].type(RespType::Error);
      pending_response_->asArray()[response_index].asString() = "upstream protocol error";
      error_count_++;
    }
    break;
  }
  case RespType::Error:
  case RespType::BulkString: {
    for (const uint32_t response_index : response_indexes) {
      pending_response_->asArray()[response_index].type(value->type());
      pending_response_->asArray()[response_index].asString().swap(value->asString());
      error_count_++;
    }
    break;
  }
  case RespType::Array: {
    ASSERT(response_indexes.size() == value->asArray().size());
    for (uint32_t i = 0; i < response_indexes.size(); i++) {
      RespValue& nested_value = value->asArray()[i];

      pending_response_->asArray()[response_indexes[i]].type(nested_value.type());
      switch (nested_value.type()) {
        case RespType::Null:
          break;
        case RespType::BulkString: {
          pending_response_->asArray()[response_indexes[i]].asString().swap(nested_value.asString());
          break;
        }
        default:
          NOT_REACHED;
      }
    }
    break;
  }
  }

  ASSERT(num_pending_responses_ > 0);
  if (--num_pending_responses_ == 0) {
    ENVOY_LOG(debug, "redis: response: '{}'", pending_response_->toString());
    callbacks_.onResponse(std::move(pending_response_));
  }
}

SplitRequestPtr MSETRequest::create(ConnPool::Instance& conn_pool,
                                    const RespValue& incoming_request, SplitCallbacks& callbacks)
                                    {
  if ((incoming_request.asArray().size() - 1) % 2 != 0) {
    onWrongNumberOfArguments(callbacks, incoming_request);
    return nullptr;
  }

  // Generate a map of hosts to keys and their original index in the request.
  std::unordered_map<std::string, std::vector<std::tuple<std::string, std::string, uint32_t>>> request_map;
  for (uint32_t i = 1; i < incoming_request.asArray().size(); i += 2) {
    const std::string& key = incoming_request.asArray()[i].asString();
    const std::string& value = incoming_request.asArray()[i + 1].asString();
    const std::string& host = conn_pool.getHost(key);
    
    std::tuple<std::string, std::string, uint32_t> command_and_index {key, value, i - 1}; 
    
    auto collapsed_request = request_map.find(host);
    if (collapsed_request != request_map.end()) {
      collapsed_request->second.push_back(command_and_index);
    } else {
      request_map[host] = {command_and_index};
    }
  }

  // Initialize empty request.
  std::unique_ptr<MSETRequest> request_ptr{new MSETRequest(callbacks)};
  request_ptr->num_pending_responses_ = request_map.size();
  request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

  // Initalize empty response.
  request_ptr->pending_response_.reset(new RespValue());
  request_ptr->pending_response_->type(RespType::SimpleString);
  
  RespValue mset; 
  mset.type(RespType::Array);

  uint32_t request_index{};
  for (const auto& request : request_map) {
    const std::vector<std::tuple<std::string, std::string, uint32_t>>& command_index_pairs = request.second;

    std::vector<RespValue> collapsed_request((command_index_pairs.size() * 2) + 1);
    collapsed_request[0].type(RespType::BulkString);
    collapsed_request[0].asString() = "MSET";
    
    std::vector<uint32_t> response_indexes;
    response_indexes.reserve(command_index_pairs.size());
    for (uint32_t i = 0; i < command_index_pairs.size(); i++) {
      const uint32_t key_index = (2 * i) + 1;
      collapsed_request[key_index].type(RespType::BulkString);
      collapsed_request[key_index].asString() = std::get<0>(command_index_pairs[i]);
      collapsed_request[key_index + 1].type(RespType::BulkString);
      collapsed_request[key_index + 1].asString() = std::get<1>(command_index_pairs[i]);
      response_indexes.push_back(std::get<2>(command_index_pairs[i]));
    }
    mset.asArray().swap(collapsed_request);

    request_ptr->pending_requests_.emplace_back(*request_ptr, request_index++, response_indexes);
    PendingRequest& pending_request = request_ptr->pending_requests_.back();

    pending_request.handle_ = conn_pool.makeRequest(mset.asArray()[1].asString(), mset, pending_request);
    if (!pending_request.handle_) {
      pending_request.onResponse(Utility::makeError("no upstream host"));
    }
  }

  return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
}

void MSETRequest::onChildResponse(RespValuePtr&& value, uint32_t index, std::vector<uint32_t> response_indexes) {
  pending_requests_[index].handle_ = nullptr;

  switch (value->type()) {
  case RespType::SimpleString: {
    if (value->asString() == "OK") {
      break;
    }
    FALLTHRU;
  }
  default: {
    error_count_+= response_indexes.size();
    break;
  }
  }

  ASSERT(num_pending_responses_ > 0);
  if (--num_pending_responses_ == 0) {
    if (error_count_ == 0) {
      pending_response_->asString() = "OK";
      callbacks_.onResponse(std::move(pending_response_));
    } else {
      callbacks_.onResponse(
          Utility::makeError(fmt::format("finished with {} error(s)", error_count_)));
    }
  }
}

// SplitRequestPtr SplitKeysSumResultRequest::create(ConnPool::Instance& conn_pool,
//                                                   const RespValue& incoming_request,
//                                                   SplitCallbacks& callbacks) {
//   std::unique_ptr<SplitKeysSumResultRequest> request_ptr{new
//   SplitKeysSumResultRequest(callbacks)};

//   request_ptr->num_pending_responses_ = incoming_request.asArray().size() - 1;
//   request_ptr->pending_requests_.reserve(request_ptr->num_pending_responses_);

//   request_ptr->pending_response_.reset(new RespValue());
//   request_ptr->pending_response_->type(RespType::Integer);

//   std::vector<RespValue> values(2);
//   values[0].type(RespType::BulkString);
//   values[0].asString() = incoming_request.asArray()[0].asString();
//   values[1].type(RespType::BulkString);
//   RespValue single_fragment;
//   single_fragment.type(RespType::Array);
//   single_fragment.asArray().swap(values);

//   for (uint32_t i = 1; i < incoming_request.asArray().size(); i++) {
//     request_ptr->pending_requests_.emplace_back(*request_ptr, i - 1);
//     PendingRequest& pending_request = request_ptr->pending_requests_.back();

//     single_fragment.asArray()[1].asString() = incoming_request.asArray()[i].asString();
//     ENVOY_LOG(debug, "redis: parallel {}: '{}'", incoming_request.asArray()[0].asString(),
//               single_fragment.toString());
//     pending_request.handle_ = conn_pool.makeRequest(incoming_request.asArray()[i].asString(),
//                                                     single_fragment, pending_request);
//     if (!pending_request.handle_) {
//       pending_request.onResponse(Utility::makeError("no upstream host"));
//     }
//   }

//   return request_ptr->num_pending_responses_ > 0 ? std::move(request_ptr) : nullptr;
// }

// void SplitKeysSumResultRequest::onChildResponse(RespValuePtr&& value, uint32_t index) {
//   pending_requests_[index].handle_ = nullptr;

//   switch (value->type()) {
//   case RespType::Integer: {
//     total_ += value->asInteger();
//     break;
//   }
//   default: {
//     error_count_++;
//     break;
//   }
//   }

//   ASSERT(num_pending_responses_ > 0);
//   if (--num_pending_responses_ == 0) {
//     if (error_count_ == 0) {
//       pending_response_->asInteger() = total_;
//       callbacks_.onResponse(std::move(pending_response_));
//     } else {
//       callbacks_.onResponse(
//           Utility::makeError(fmt::format("finished with {} error(s)", error_count_)));
//     }
//   }
// }

InstanceImpl::InstanceImpl(ConnPool::InstancePtr&& conn_pool, Stats::Scope& scope,
                           const std::string& stat_prefix)
    : conn_pool_(std::move(conn_pool)), simple_command_handler_(*conn_pool_),
      eval_command_handler_(*conn_pool_),
      mget_handler_(*conn_pool_), mset_handler_(*conn_pool_), stats_{ALL_COMMAND_SPLITTER_STATS(
                                      POOL_COUNTER_PREFIX(scope, stat_prefix + "splitter."))} {

  // InstanceImpl::InstanceImpl(ConnPool::InstancePtr&& conn_pool, Stats::Scope& scope,
  //                            const std::string& stat_prefix)
  //     : conn_pool_(std::move(conn_pool)), simple_command_handler_(*conn_pool_),
  //       eval_command_handler_(*conn_pool_), mget_handler_(*conn_pool_),
  //       mset_handler_(*conn_pool_), split_keys_sum_result_handler_(*conn_pool_),
  //       stats_{ALL_COMMAND_SPLITTER_STATS(POOL_COUNTER_PREFIX(scope, stat_prefix + "splitter."))}
  //       {

  // TODO(mattklein123) PERF: Make this a trie (like in header_map_impl).
  for (const std::string& command : SupportedCommands::simpleCommands()) {
    addHandler(scope, stat_prefix, command, simple_command_handler_);
  }

  for (const std::string& command : SupportedCommands::evalCommands()) {
    addHandler(scope, stat_prefix, command, eval_command_handler_);
  }

  // for (const std::string& command : SupportedCommands::hashMultipleSumResultCommands()) {
  //   addHandler(scope, stat_prefix, command, split_keys_sum_result_handler_);
  // }

  addHandler(scope, stat_prefix, SupportedCommands::mget(), mget_handler_);
  addHandler(scope, stat_prefix, SupportedCommands::mset(), mset_handler_);
}

SplitRequestPtr InstanceImpl::makeRequest(const RespValue& request, SplitCallbacks& callbacks) {
  if (request.type() != RespType::Array || request.asArray().size() < 2) {
    onInvalidRequest(callbacks);
    return nullptr;
  }

  for (const RespValue& value : request.asArray()) {
    if (value.type() != RespType::BulkString) {
      onInvalidRequest(callbacks);
      return nullptr;
    }
  }

  std::string to_lower_string(request.asArray()[0].asString());
  to_lower_table_.toLowerCase(to_lower_string);

  auto handler = command_map_.find(to_lower_string);
  if (handler == command_map_.end()) {
    stats_.unsupported_command_.inc();
    callbacks.onResponse(Utility::makeError(
        fmt::format("unsupported command '{}'", request.asArray()[0].asString())));
    return nullptr;
  }

  ENVOY_LOG(debug, "redis: splitting '{}'", request.toString());
  handler->second.total_.inc();
  return handler->second.handler_.get().startRequest(request, callbacks);
}

void InstanceImpl::onInvalidRequest(SplitCallbacks& callbacks) {
  stats_.invalid_request_.inc();
  callbacks.onResponse(Utility::makeError("invalid request"));
}

void InstanceImpl::addHandler(Stats::Scope& scope, const std::string& stat_prefix,
                              const std::string& name, CommandHandler& handler) {
  std::string to_lower_name(name);
  to_lower_table_.toLowerCase(to_lower_name);
  command_map_.emplace(
      to_lower_name,
      HandlerData{scope.counter(fmt::format("{}command.{}.total", stat_prefix, to_lower_name)),
                  handler});
}

} // namespace CommandSplitter
} // namespace Redis
} // namespace Envoy
