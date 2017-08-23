#include "balancer.hpp"

#define LB_VERBOSE 0
#if LB_VERBOSE
#define LBOUT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define LBOUT(fmt, ...) /** **/
#endif

Balancer::Balancer(
       netstack_t& incoming, uint16_t in_port,
       netstack_t& outgoing, std::vector<net::Socket> nodelist)
  : netin(incoming), nodes()
{
  for (auto& addr : nodelist) {
    nodes.add_node(outgoing, addr, pool_signal_t{this, &Balancer::handle_queue});
  }

  netin.tcp().listen(in_port,
  [this] (tcp_ptr conn) {
    if (conn == nullptr) return;
    this->incoming(conn);
  });
}

int Balancer::wait_queue() const {
  return queue.size();
}
void Balancer::incoming(tcp_ptr conn)
{
    queue.emplace_back(conn);
    LBOUT("Queueing connection (q=%lu)\n", queue.size());
    this->handle_queue();
}
void Balancer::handle_queue()
{
  // check waitq
  while (nodes.pool_size() > 0 && queue.empty() == false)
  {
    auto item = std::move(queue.front());
    queue.pop_front();
    if (item.conn->is_connected()) {
      // NOTE: explicitly want to copy buffers
      if (nodes.assign(item.conn, item.buffers) == false) {
        queue.push_back(std::move(item));
        break;
      }
    }
  } // waitq check
  // check if we need to create more connections
  this->handle_connections();
}
void Balancer::handle_connections()
{
  int estimate = queue.size() - (nodes.pool_connecting() + nodes.pool_size());
  if (estimate > 0)
  {
    const int MAX_ATTEMPTS = 100;
    estimate = std::min(nodes.pool_connecting() + estimate, MAX_ATTEMPTS);
    nodes.create_connections(estimate);
  }
}

Waiting::Waiting(tcp_ptr incoming)
  : conn(incoming), total(0)
{
  // queue incoming data from clients not yet
  // assigned to a node
  conn->on_read(READQ_PER_CLIENT,
  [this] (auto buf, size_t len) {
    // prevent buffer bloat attack
    total += len;
    if (total > MAX_READQ_PER_NODE) {
      conn->abort();
    }
    else {
      LBOUT("*** Queued %lu bytes\n", len);
      buffers.emplace_back(buf, len);
    }
  });
}

void Nodes::create_connections(int total)
{
  // temporary iterator
  for (int i = 0; i < total; i++)
  {
    nodes[conn_iterator].connect();
    conn_iterator = (conn_iterator + 1) % nodes.size();
  }
}
bool Nodes::assign(tcp_ptr conn, queue_vector_t readq)
{
  for (size_t i = 0; i < nodes.size(); i++)
  {
    auto outgoing = nodes[algo_iterator].get_connection();
    // algorithm here //
    algo_iterator = (algo_iterator + 1) % nodes.size();
    // check if connection was retrieved
    if (outgoing != nullptr)
    {
      assert(outgoing->is_connected());
      LBOUT("Assigning client to node %d (%s)\n",
            iterator, outgoing->to_string().c_str());
      this->create_session(conn, outgoing);
      // flush readq
      for (auto& buffer : readq) {
        LBOUT("*** Flushing %lu bytes\n", buffer.second);
        outgoing->write(std::move(buffer.first), buffer.second);
      }
      return true;
    }
  }
  return false;
}
int Nodes::pool_connecting() const {
  int count = 0;
  for (auto& node : nodes) count += node.connection_attempts();
  return count;
}
int Nodes::pool_size() const {
  int count = 0;
  for (auto& node : nodes) count += node.pool_size();
  return count;
}
int64_t Nodes::total_sessions() const {
  return session_total;
}
int Nodes::open_sessions() const {
  return session_cnt;
}
void Nodes::create_session(tcp_ptr client, tcp_ptr outgoing)
{
  int idx = -1;
  if (free_sessions.empty()) {
    idx = sessions.size();
    sessions.emplace_back(*this, idx, client, outgoing);
  } else {
    idx = free_sessions.back();
    new (&sessions[idx]) Session(*this, idx, client, outgoing);
    free_sessions.pop_back();
  }
  session_total++;
  session_cnt++;
  LBOUT("New session %d  (current = %d, total = %ld)\n",
        idx, session_cnt, session_total);
}
Session& Nodes::get_session(int idx)
{
  return sessions.at(idx);
}
void Nodes::close_session(int idx)
{
  auto& session = get_session(idx);
  // disable timeout timer
  if (session.timeout_timer != Timers::UNUSED_ID) {
    Timers::stop(session.timeout_timer);
    session.timeout_timer = Timers::UNUSED_ID;
  }
  // close connections
  if (session.incoming != nullptr) {
    auto conn = std::move(session.incoming);
    conn->reset_callbacks();
    if (conn->is_connected()) conn->close();
  }
  if (session.outgoing != nullptr) {
    auto conn = std::move(session.outgoing);
    conn->reset_callbacks();
    if (conn->is_connected()) conn->close();
  }
  // free session
  free_sessions.push_back(session.self);
  session_cnt--;
  LBOUT("Session %d closed  (total = %d)\n", session.self, session_cnt);
}

void Node::connect()
{
  auto outgoing = this->stack.tcp().connect(this->addr);
  // connecting to node atm.
  this->connecting++;
  // retry timer when connect takes too long
  int fail_timer = Timers::oneshot(CONNECT_WAIT_PERIOD,
  [this, outgoing] (int)
  {
    // close connection
    outgoing->abort();
    // no longer connecting
    assert(this->connecting > 0);
    this->connecting --;
    // retry connection?
    Timers::oneshot(CONNECT_RETRY_WAIT_TIME,
      [] (this) {
        this->connect();
      });
  });
  // add connection to pool on success, otherwise.. retry
  outgoing->on_connect(
  [this, fail_timer] (auto conn)
  {
    // stop retry timer
    Timers::stop(fail_timer);
    // no longer connecting
    assert(this->connecting > 0);
    this->connecting --;
    // connection may be null, apparently
    if (conn != nullptr)
    {
      LBOUT("Connected to %s  (%ld total)\n",
              addr.to_string().c_str(), pool.size());
      this->pool.push_back(conn);
      // signal change in pool
      this->pool_signal();
    }
    else {
      // signal change in pool
      this->pool_signal();
    }
  });
}
tcp_ptr Node::get_connection()
{
  while (pool.empty() == false) {
      auto conn = pool.back();
      assert(conn != nullptr);
      pool.pop_back();
      if (conn->is_connected()) return conn;
      else conn->close();
  }
  return nullptr;
}

// use indexing to access Session because std::vector
Session::Session(Nodes& n, int idx, tcp_ptr inc, tcp_ptr out)
    : parent(n), self(idx), incoming(inc), outgoing(out)
{
  using namespace std::chrono;
  this->timeout_timer = Timers::oneshot(INITIAL_SESSION_TIMEOUT,
  [&nodes = n, idx] (int) {
    nodes.close_session(idx);
  });
  incoming->on_read(READQ_PER_CLIENT,
  [&nodes = n, idx] (auto buf, size_t len) mutable {
      auto& session = nodes.get_session(idx);
      session.handle_timeout();
      session.outgoing->write(std::move(buf), len);
  });
  incoming->on_close(
  [&nodes = n, idx] () mutable {
      nodes.close_session(idx);
  });
  outgoing->on_read(READQ_FOR_NODES,
  [&nodes = n, idx] (auto buf, size_t len) mutable {
      auto& session = nodes.get_session(idx);
      session.handle_timeout();
      session.incoming->write(std::move(buf), len);
  });
  outgoing->on_close(
  [&nodes = n, idx] () mutable {
      nodes.close_session(idx);
  });
}
void Session::handle_timeout()
{
  // stop old timer
  Timers::stop(this->timeout_timer);
  // create new timeout
  using namespace std::chrono;
  this->timeout_timer = Timers::oneshot(ROLLING_SESSION_TIMEOUT,
  [&nodes = parent, idx = self] (int) {
    nodes.close_session(idx);
  });
}
