// Copyright (c) Microsoft Corporation.
// Copyright (c) 1999 Miguel Castro, Barbara Liskov.
// Copyright (c) 2000, 2001 Miguel Castro, Rodrigo Rodrigues, Barbara Liskov.
// Licensed under the MIT license.

#include "node.h"

#include "crypt.h"
#include "ds/ccf_assert.h"
#include "ds/logger.h"
#include "itimer.h"
#include "message.h"
#include "message_tags.h"
#include "parameters.h"
#include "principal.h"
#include "time_types.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NDEBUG
#  define NDEBUG
#endif

Node::Node(const NodeInfo& node_info_) : node_info(node_info_)
{
  replica_count = 0;

  key_pair =
    std::make_unique<tls::KeyPair>(tls::parse_private_key(node_info.privk));

  service_name = node_info.general_info.service_name;

  // read max_faulty and compute derived variables
  max_faulty = node_info.general_info.max_faulty;
  num_replicas = node_info.general_info.num_replicas;
  if (num_replicas <= 2 * max_faulty)
  {
    LOG_FATAL_FMT(
      "Not enough replicas: {} for desired f: {}", num_replicas, max_faulty);
    throw std::logic_error(
      "Not enough replicas: " + std::to_string(num_replicas) +
      " for desired f: " + std::to_string(max_faulty));
  }
  num_clients = node_info.general_info.num_clients;
  if (num_replicas > Max_num_replicas)
  {
    PBFT_FAIL("Invalid number of replicas");
  }
  if (max_faulty == 0)
  {
    // f == 0 so num_correct_replicas is set to 1
    // Certificates containing only the replica's
    // own certificate are considered complete
    threshold = 1;
  }
  else
  {
    threshold = num_replicas - max_faulty;
  }

  LOG_INFO_FMT("Max faulty (f): {} num replicas: {}", max_faulty, num_replicas);

  // Read authentication timeout
  int at = node_info.general_info.auth_timeout;

  node_id = -1;
  node_id = node_info.own_info.id;

  add_principal(node_info.own_info);

  if (node_id < 0)
  {
    PBFT_FAIL("Could not find my principal");
  }

  // Initialize current view number and primary.
  v = 0;
  cur_primary = 0;

#ifndef INSIDE_ENCLAVE
  // Sleep for more than a second to ensure strictly increasing
  // timestamps.
  sleep(2);
#endif

  send_only_to_self = ((f() == 0 && is_replica(id())));
}

void Node::add_principal(const PrincipalInfo& principal_info)
{
  LOG_INFO_FMT("Adding principal with id:{}", principal_info.id);
  auto principals = get_principals();
  auto it = principals->find(principal_info.id);
  if (it != principals->end())
  {
    LOG_INFO_FMT(
      "Principal with id:{} has already been configured", principal_info.id);
    auto& principal = it->second;
    if (principal->get_cert().empty())
    {
      principal->set_certificate(principal_info.cert);
    }
    return;
  }
  Addr a;
  bzero((char*)&a, sizeof(a));
  a.sin_family = AF_INET;

#ifndef INSIDE_ENCLAVE
  a.sin_addr.s_addr = inet_addr(principal_info.ip.c_str());
  a.sin_port = htons(principal_info.port);
#endif
  auto new_principals = std::make_shared<Principal_map>(*principals);

  new_principals->insert(
    {principal_info.id,
     std::make_shared<Principal>(
       principal_info.id, a, principal_info.is_replica, principal_info.cert)});

  std::atomic_store(&atomic_principals, new_principals);

  LOG_INFO_FMT("Added principal with id:{}", principal_info.id);

  if (principal_info.is_replica)
  {
    replica_count++;
  }
}

void Node::configure_principals()
{
  for (auto& pi : node_info.general_info.principal_info)
  {
    if (pi.id != node_id)
    {
      LOG_INFO_FMT("Adding principal:{}", pi.id);
      add_principal(pi);
    }
  }
}

void Node::init_network(std::unique_ptr<INetwork>&& network_)
{
  auto principals = get_principals();
  LOG_INFO_FMT("Principals - count:{}", principals->size());
  network = std::move(network_);
  auto it = principals->find(node_id);
  assert(it != principals->end());
  network->Initialize(it->second->address()->sin_port);
}

void Node::send(Message* m, int i)
{
  if (i == All_replicas)
  {
    send_to_replicas(m);
    return;
  }

  std::shared_ptr<Principal> p = get_principal(i);
  if (p == nullptr)
  {
    // principal not ready yet!
    return;
  }

  send(m, p.get());
}

void Node::send(Message* m, Principal* p)
{
  CCF_ASSERT(m->tag() < Max_message_tag, "Invalid message tag");
  CCF_ASSERT(p != nullptr, "Must send to a principal");

  int error = 0;
  int size = m->size();
  while (error < size)
  {
    if (!network)
    {
      throw std::logic_error("Network not set");
    }

    error = network->Send(m, *p);

#ifndef NDEBUG
    if (error < 0 && error != EAGAIN)
      perror("Node::send: sendto");
#endif
  }
}

bool Node::has_messages(long to)
{
  if (!network)
  {
    throw std::logic_error("Network not set");
  }
  return network->has_messages(to);
}

size_t Node::gen_signature(const char* src, unsigned src_len, char* sig)
{
  auto signature = key_pair->sign(CBuffer{(uint8_t*)src, src_len});
  std::copy(signature.begin(), signature.end(), sig);

  return signature.size();
}

size_t Node::gen_signature(
  const char* src, unsigned src_len, PbftSignature& sig)
{
  size_t sig_size;
  key_pair->sign(CBuffer{(uint8_t*)src, src_len}, &sig_size, sig.data());
  assert(sig_size <= sig.size());

  return sig_size;
}

void Node::copy_signature(const PbftSignature& signature, PbftSignature& dest)
{
  std::copy(signature.begin(), signature.end(), dest.begin());
  std::fill(
    dest.end(),
    dest.end() + ALIGNED_SIZE(pbft_max_signature_size) -
      pbft_max_signature_size,
    0);
}

Request_id Node::new_rid()
{
  return request_id_generator.next_rid();
}

void Node::send_to_replicas(Message* m)
{
  LOG_TRACE_FMT(
    "replica_count:{}, num_replicas:{}, m:{}",
    replica_count,
    num_replicas,
    m->tag());

  if (send_only_to_self && m->tag() != Status_tag)
  {
    LOG_TRACE_FMT("Only sending to self");
    send(m, node_id);
  }
  else
  {
    auto principals = get_principals();
    for (auto& it : *principals)
    {
      if (it.second->is_replica() && it.second->pid() != node_id)
      {
        send(m, it.second.get());
      }
    }
  }
}

void Node::set_f(size_t f)
{
  LOG_INFO_FMT("***** setting f to {} *****", f);
  max_faulty = f;
  send_only_to_self = (f == 0);
  threshold = f * 2 + 1;
  num_replicas = 3 * f + 1;
}