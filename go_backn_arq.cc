#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"
#include "ns3/seq-ts-header.h"

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("GoBackNExample");

// Simple Go-Back-N sender
class GoBackNSender : public Application {
public:
  GoBackNSender();
  void Setup(Ptr<Socket> socket, Address peer, uint32_t totalPackets, Time timeout, uint32_t windowSize);

private:
  virtual void StartApplication();
  virtual void StopApplication();
  void SendWindow();
  void Timeout();
  void HandleAck(Ptr<Socket> socket);

  Ptr<Socket> m_socket;
  Address m_peer;
  uint32_t m_totalPackets;
  uint32_t m_windowSize;
  Time m_timeout;
  uint32_t m_base;
  uint32_t m_nextSeq;
  std::map<uint32_t, EventId> m_timers;
  EventId m_timeoutEvent;
  std::set<uint32_t> m_acked;
};

GoBackNSender::GoBackNSender()
    : m_socket(0), m_totalPackets(0), m_windowSize(0), m_base(0), m_nextSeq(0) {}

void GoBackNSender::Setup(Ptr<Socket> socket, Address peer, uint32_t totalPackets, Time timeout, uint32_t windowSize) {
  m_socket = socket;
  m_peer = peer;
  m_totalPackets = totalPackets;
  m_timeout = timeout;
  m_windowSize = windowSize;
}

void GoBackNSender::StartApplication() {
  m_socket->Connect(m_peer);
  m_socket->SetRecvCallback(MakeCallback(&GoBackNSender::HandleAck, this));
  SendWindow();
}

void GoBackNSender::StopApplication() {
  if (m_socket)
    m_socket->Close();
}

void GoBackNSender::SendWindow() {
  while (m_nextSeq < m_base + m_windowSize && m_nextSeq < m_totalPackets) {
    Ptr<Packet> pkt = Create<Packet>(100);
    SeqTsHeader hdr;
    hdr.SetSeq(m_nextSeq);
    pkt->AddHeader(hdr);
    m_socket->Send(pkt);
    NS_LOG_INFO("Sender: Sent packet " << m_nextSeq);
    m_nextSeq++;
  }
  if (!m_timeoutEvent.IsRunning())
    m_timeoutEvent = Simulator::Schedule(m_timeout, &GoBackNSender::Timeout, this);
}

void GoBackNSender::Timeout() {
  NS_LOG_INFO("Timeout! Resending window from " << m_base);
  m_nextSeq = m_base;
  SendWindow();
}

void GoBackNSender::HandleAck(Ptr<Socket> socket) {
  Ptr<Packet> packet = socket->Recv();
  SeqTsHeader hdr;
  packet->RemoveHeader(hdr);
  uint32_t ack = hdr.GetSeq();
  NS_LOG_INFO("Sender: Got ACK " << ack);

  if (ack >= m_base) {
    m_base = ack + 1;
    if (m_base == m_nextSeq)
      Simulator::Cancel(m_timeoutEvent);
    else {
      Simulator::Cancel(m_timeoutEvent);
      m_timeoutEvent = Simulator::Schedule(m_timeout, &GoBackNSender::Timeout, this);
    }
  }

  if (m_base < m_totalPackets)
    SendWindow();
}

// Receiver side
class GoBackNReceiver : public Application {
public:
  GoBackNReceiver();
  void Setup(Ptr<Socket> socket);
private:
  virtual void StartApplication();
  void HandleRead(Ptr<Socket> socket);
  Ptr<Socket> m_socket;
  uint32_t m_expected;
};

GoBackNReceiver::GoBackNReceiver() : m_socket(0), m_expected(0) {}

void GoBackNReceiver::Setup(Ptr<Socket> socket) { m_socket = socket; }

void GoBackNReceiver::StartApplication() {
  m_socket->SetRecvCallback(MakeCallback(&GoBackNReceiver::HandleRead, this));
}

void GoBackNReceiver::HandleRead(Ptr<Socket> socket) {
  Ptr<Packet> pkt = socket->Recv();
  SeqTsHeader hdr;
  pkt->RemoveHeader(hdr);
  uint32_t seq = hdr.GetSeq();

  if (seq == m_expected) {
    NS_LOG_INFO("Receiver: Got packet " << seq);
    m_expected++;
  } else {
    NS_LOG_INFO("Receiver: Got out-of-order packet " << seq << " (expected " << m_expected << ")");
  }

  // Send ACK for last correctly received
  Ptr<Packet> ack = Create<Packet>();
  SeqTsHeader ackHdr;
  ackHdr.SetSeq(m_expected - 1);
  ack->AddHeader(ackHdr);
  socket->Send(ack);
  NS_LOG_INFO("Receiver: Sent ACK " << (m_expected - 1));
}

int main(int argc, char *argv[]) {
  LogComponentEnable("GoBackNExample", LOG_LEVEL_INFO);

  NodeContainer nodes;
  nodes.Create(2);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("1Mbps"));
  p2p.SetChannelAttribute("Delay", StringValue("10ms"));

  NetDeviceContainer devices = p2p.Install(nodes);
  InternetStackHelper stack;
  stack.Install(nodes);

  Ipv4AddressHelper address;
  address.SetBase("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfaces = address.Assign(devices);

  TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");

  Ptr<Socket> recvSocket = Socket::CreateSocket(nodes.Get(1), tid);
  InetSocketAddress recvAddr = InetSocketAddress(Ipv4Address::GetAny(), 8080);
  recvSocket->Bind(recvAddr);
  recvSocket->Connect(InetSocketAddress(interfaces.GetAddress(0), 8081));

  Ptr<GoBackNReceiver> receiver = CreateObject<GoBackNReceiver>();
  receiver->Setup(recvSocket);
  nodes.Get(1)->AddApplication(receiver);
  receiver->SetStartTime(Seconds(0.0));

  Ptr<Socket> sendSocket = Socket::CreateSocket(nodes.Get(0), tid);
  InetSocketAddress senderAddr = InetSocketAddress(Ipv4Address::GetAny(), 8081);
  sendSocket->Bind(senderAddr);

  Ptr<GoBackNSender> sender = CreateObject<GoBackNSender>();
  sender->Setup(sendSocket, InetSocketAddress(interfaces.GetAddress(1), 8080), 10, Seconds(2.0), 4);
  nodes.Get(0)->AddApplication(sender);
  sender->SetStartTime(Seconds(1.0));

  // Animation
  AnimationInterface anim("gobackn-arq.xml");
  anim.SetConstantPosition(nodes.Get(0), 10, 20);
  anim.SetConstantPosition(nodes.Get(1), 50, 20);
  anim.UpdateNodeDescription(nodes.Get(0), "Sender");
  anim.UpdateNodeDescription(nodes.Get(1), "Receiver");

  Simulator::Run();
  Simulator::Destroy();
  return 0;
}
