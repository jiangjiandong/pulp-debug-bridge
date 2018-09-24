/*
 * Copyright (C) 2018 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Authors: Germain Haugou, ETH (germain.haugou@iis.ee.ethz.ch)
 */

#include "jtag-proxy/jtag-proxy.hpp"

Jtag_proxy::Jtag_proxy(EventLoop::SpEventLoop event_loop, cable_cb_t cable_state_cb) : log("JTAG"), cable_state_cb(cable_state_cb)
{
  using namespace std::placeholders;
  m_tcp_client = std::make_shared<Tcp_client>(&log, event_loop);
  m_tcp_client->set_connected_cb(std::bind(&Jtag_proxy::client_connected, this, _1));
  m_tcp_client->set_disconnected_cb(std::bind(&Jtag_proxy::client_disconnected, this, _1));
}   

void Jtag_proxy::client_connected(Tcp_socket::tcp_socket_ptr_t socket)
{
  if (socket == nullptr) {
    log.user("JTAG Proxy: Connection to (%s:%d) timed out - retrying\n", m_server, m_port);
    m_tcp_client->connect(m_server, m_port);
  } else {
    log.user("JTAG Proxy: Connected to (%s:%d)\n", m_server, m_port);
    cable_state_cb(CABLE_CONNECTED);
  }
}

void Jtag_proxy::client_disconnected(Tcp_socket::tcp_socket_ptr_t)
{
  log.user("JTAG Proxy: Disconnected from (%s:%d)\n", m_server, m_port);
  m_socket = nullptr;
  cable_state_cb(CABLE_DISCONNECTED);
}

bool Jtag_proxy::connect(js::config *config)
{
  js::config *proxy_config = config->get("jtag-proxy");

  if (proxy_config == NULL || proxy_config->get("port") == NULL)
  {
    log.error("Didn't find any information for JTAG proxy\n");
    return false;
  }

  m_port = proxy_config->get("port")->get_int();

  if (proxy_config->get("host") == NULL)
    m_server = "localhost";
  else
    m_server = proxy_config->get("host")->get_str().c_str();

  log.user("JTAG Proxy: Connecting to (%s:%d)\n", m_server, m_port);

  m_socket = m_tcp_client->connect_blocking(m_server, m_port, 10000000L);
  return m_socket != nullptr;
}

bool Jtag_proxy::bit_inout(char* inbit, char outbit, bool last)
{
  return stream_inout(inbit, &outbit, 1, last);
}

bool Jtag_proxy::proxy_stream(char* instream, char* outstream, unsigned int n_bits, bool last, int bit)
{
  if (m_socket == nullptr) return false;
  proxy_req_t req;
  req.type=DEBUG_BRIDGE_JTAG_REQ;
  req.jtag.bits = n_bits;
  req.jtag.tdo = instream != NULL;

  if (n_bits >= (1<<16)) return false;

  std::vector<uint8_t> buffer(n_bits, 0);
  uint8_t value = 0;
  for (size_t i=0; i<n_bits; i++)
  {
    if ((i % 8) == 0)
    {
      if (outstream)
      {
        value = *(unsigned char *)outstream;
        outstream++;
      }
      else
        value = 0;
    }

    buffer[i] = (value & 1) << bit;
    if (bit != DEBUG_BRIDGE_JTAG_TRST) buffer[i] |= 1 << DEBUG_BRIDGE_JTAG_TRST;

    value >>= 1;
  }

  if (last)
  {
    buffer[n_bits-1] |= 1 << DEBUG_BRIDGE_JTAG_TMS;
  }

  if (m_socket->write_immediate((void *) &req, sizeof(req), true) != sizeof(req)) return false;
  if (m_socket->write_immediate((void *)(&(buffer[0])), n_bits, true) != n_bits) return false;
  if (instream != NULL)
  {
    ::memset((void *)instream, 0, (n_bits + 7) / 8);
    if (m_socket->read_immediate((void *)instream, (n_bits + 7) / 8, true) != (func_ret_t)((n_bits + 7) / 8))
      return false;
  }
  return true;
}

bool Jtag_proxy::stream_inout(char* instream, char* outstream, unsigned int n_bits, bool last)
{
  return proxy_stream(instream, outstream, n_bits, last, DEBUG_BRIDGE_JTAG_TDI);
}

bool Jtag_proxy::jtag_reset(bool active)
{
  int value = !active;
  return proxy_stream(NULL, (char *)&value, 1, 0, DEBUG_BRIDGE_JTAG_TRST);
}

int Jtag_proxy::flush()
{
  return true;
}

bool Jtag_proxy::chip_reset(bool active)
{
  if (m_socket == nullptr) return false;
  proxy_req_t req;
  req.type=DEBUG_BRIDGE_RESET_REQ;
  req.reset.active = active;
  if (m_socket->write_immediate((void *)&req, sizeof(req), true) != sizeof(req)) return false;
  return true;
}
