/* stop-and-wait.cc
   Simple Stop-and-Wait ARQ demo (ns-3)
   Compile: put in ns-3/scratch/ and run: ./waf --run scratch/stop-and-wait
*/

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("StopAndWaitDemo");

static const uint16_t DATA_PORT = 9000;
static const uint16_t ACK_PORT  = 9001;

class SWSender : public Application
{
public:
  SWSender () : m_socket(0), m_peer(), m_seq(0), m_waitingAck(false) {}
  virtual ~SWSender() { m_socket = 0; }

  void Setup (Address peer, Time timeout, uint32_t totalPackets, Time interPacket)
  {
    m_peer = peer;
    m_timeout = timeout;
    m_totalPackets = totalPackets;
    m_interPacket = interPacket;
  }

private:
  virtual void StartApplication() override
  {
    if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode(), UdpSocketFactory::GetTypeId ());
      InetSocketAddress local = InetSocketAddress (Ipv4Address::GetAny (), ACK_PORT);
      m_socket->Bind (local);
      m_socket->SetRecvCallback (MakeCallback (&SWSender::HandleRead, this));
    }
    SendNewPacket ();
  }

  virtual void StopApplication() override
  {
    if (m_socket) {
      m_socket->Close ();
    }
    Simulator::Cancel (m_retxEvent);
  }

  void SendNewPacket ()
  {
    if (m_seqCount >= m_totalPackets)
      return;

    if (m_waitingAck) return;

    // Build a small packet: 4 bytes seq + payload text
    uint32_t seqnum = m_seq;
    Ptr<Packet> p = Create<Packet> ((uint8_t*)&seqnum, sizeof(seqnum)); // raw bytes
    // add a small payload string
    std::string payload = "DATA";
    p->AddAtEnd (Create<Packet> ((const uint8_t*)payload.c_str(), payload.size()));

    int rv = m_socket->SendTo (p, 0, m_peer);
    NS_LOG_INFO ("Sender: Sent pkt seq=" << seqnum << " time=" << Simulator::Now ().GetSeconds ());
    m_waitingAck = true;
    // schedule timeout for retransmit
    m_retxEvent = Simulator::Schedule (m_timeout, &SWSender::Timeout, this);
  }

  void Timeout ()
  {
    NS_LOG_INFO ("Sender: Timeout for seq=" << m_seq << " at " << Simulator::Now ().GetSeconds ());
    // Retransmit the same packet (we simply call SendTo again with same seq)
    // Reconstruct same packet
    uint32_t seqnum = m_seq;
    Ptr<Packet> p = Create<Packet> ((uint8_t*)&seqnum, sizeof(seqnum));
    std::string payload = "DATA";
    p->AddAtEnd (Create<Packet> ((const uint8_t*)payload.c_str(), payload.size()));
    m_socket->SendTo (p, 0, m_peer);
    NS_LOG_INFO ("Sender: Retransmitted seq=" << seqnum << " time=" << Simulator::Now ().GetSeconds ());
    // schedule next timeout
    m_retxEvent = Simulator::Schedule (m_timeout, &SWSender::Timeout, this);
  }

  void HandleRead (Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> packet = socket->RecvFrom (from);
    if (packet->GetSize() < sizeof(uint32_t)) return;
    uint32_t ackSeq;
    packet->CopyData ((uint8_t*)&ackSeq, sizeof(ackSeq));
    NS_LOG_INFO ("Sender: Received ACK for seq=" << ackSeq << " at " << Simulator::Now ().GetSeconds ());
    if (ackSeq == m_seq && m_waitingAck)
    {
      // Got expected ACK
      m_waitingAck = false;
      ++m_seqCount;
      // toggle sequence number (for a simple 0/1 ARQ)
      m_seq = 1 - m_seq;
      // cancel retransmit timer
      Simulator::Cancel (m_retxEvent);
      // schedule sending next packet after inter-packet gap
      Simulator::Schedule (m_interPacket, &SWSender::SendNewPacket, this);
    }
    else
    {
      NS_LOG_INFO ("Sender: Unexpected ACK (got " << ackSeq << " expected " << m_seq << ")");
    }
  }

private:
  Ptr<Socket> m_socket;
  Address m_peer;
  uint32_t m_seq;            // 0/1
  uint32_t m_seqCount = 0;   // packets successfully sent
  uint32_t m_totalPackets = 10;
  bool m_waitingAck;
  EventId m_retxEvent;
  Time m_timeout;
  Time m_interPacket;
};

class SWReceiver : public Application
{
public:
  SWReceiver () : m_socket(0), m_expectedSeq(0) {}
  virtual ~SWReceiver() { m_socket = 0; }

  void Setup (uint16_t port) { m_port = port; }

private:
  virtual void StartApplication() override
  {
    if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode(), UdpSocketFactory::GetTypeId ());
      InetSocketAddress local = InetSocketAddress (Ipv4Address::GetAny (), m_port);
      m_socket->Bind (local);
      m_socket->SetRecvCallback (MakeCallback (&SWReceiver::HandleRead, this));
    }
  }

  virtual void StopApplication() override
  {
    if (m_socket) m_socket->Close ();
  }

  void HandleRead (Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> packet = socket->RecvFrom (from);
    if (packet->GetSize() < sizeof(uint32_t)) return;
    uint32_t seqnum;
    packet->CopyData ((uint8_t*)&seqnum, sizeof(seqnum));
    NS_LOG_INFO ("Receiver: Got DATA seq=" << seqnum << " time=" << Simulator::Now ().GetSeconds ());

    // If correct in-order packet:
    if (seqnum == m_expectedSeq)
    {
      NS_LOG_INFO ("Receiver: Accepting seq=" << seqnum);
      // deliver up (we just log)
      // Send ACK for seqnum
      SendAck (from, seqnum);
      // flip expected seq for next packet
      m_expectedSeq = 1 - m_expectedSeq;
    }
    else
    {
      // Duplicate or out-of-order - re-send ACK for last accepted (which is 1 - expected)
      uint32_t lastAck = 1 - m_expectedSeq;
      NS_LOG_INFO ("Receiver: Unexpected seq (got " << seqnum << "), sending ACK for last=" << lastAck);
      SendAck (from, lastAck);
    }
  }

  void SendAck (Address to, uint32_t seq)
  {
    // Build ack packet: 4 bytes seq
    Ptr<Packet> ack = Create<Packet> ((uint8_t*)&seq, sizeof(seq));
    // send to sender's ACK socket port (ACKs are sent to DATA sender's bound port)
    InetSocketAddress dst = InetSocketAddress (InetSocketAddress::ConvertFrom(to).GetIpv4 (), ACK_PORT);
    // We need a socket for sending ACKs
    if (!m_ackSocket)
    {
      m_ackSocket = Socket::CreateSocket (GetNode(), UdpSocketFactory::GetTypeId ());
      // no bind required for ephemeral port
    }
    int rv = m_ackSocket->SendTo (ack, 0, dst);
    NS_LOG_INFO ("Receiver: Sent ACK " << seq << " to " << dst.GetIpv4 () << ":" << dst.GetPort ());
  }

private:
  Ptr<Socket> m_socket;
  Ptr<Socket> m_ackSocket;
  uint16_t m_port;
  uint32_t m_expectedSeq; // 0/1
};

int main (int argc, char *argv[])
{
  Time::SetResolution (Time::NS);
  LogComponentEnable("StopAndWaitDemo", LOG_LEVEL_INFO);

  uint32_t totalPackets = 6;
  Time timeout = MilliSeconds (500);
  Time interPacket = MilliSeconds (200);

  CommandLine cmd;
  cmd.AddValue ("nPackets", "Total data packets to send", totalPackets);
  cmd.AddValue ("timeoutMs", "Retransmit timeout in ms", timeout);
  cmd.Parse (argc, argv);

  NodeContainer nodes;
  nodes.Create (2);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("2Mbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("10ms"));

  NetDeviceContainer devices = p2p.Install (nodes);

  InternetStackHelper stack;
  stack.Install (nodes);

  Ipv4AddressHelper address;
  address.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfaces = address.Assign (devices);

  // enable pcap for packet-level inspection
  p2p.EnablePcapAll ("stop-and-wait", true);

  // create sender app on node 0, receiver app on node 1
  Ptr<SWSender> senderApp = CreateObject<SWSender> ();
  Address peer = InetSocketAddress (interfaces.GetAddress (1), DATA_PORT);
  senderApp->Setup (peer, timeout, totalPackets, interPacket);
  nodes.Get (0)->AddApplication (senderApp);
  senderApp->SetStartTime (Seconds (1.0));
  senderApp->SetStopTime (Seconds (30.0));

  Ptr<SWReceiver> recvApp = CreateObject<SWReceiver> ();
  recvApp->Setup (DATA_PORT);
  nodes.Get (1)->AddApplication (recvApp);
  recvApp->SetStartTime (Seconds (0.5));
  recvApp->SetStopTime (Seconds (30.0));

  // NetAnim output
  AnimationInterface anim ("stop-and-wait.xml");
  anim.SetConstantPosition (nodes.Get (0), 0.0, 0.0);
  anim.SetConstantPosition (nodes.Get (1), 50.0, 0.0);

  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}

