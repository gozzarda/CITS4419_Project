#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstdint>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include "project.h"

using namespace std;

#define EV_TESTMSG			EV_TIMER1	// Send a test message
#define EV_HEARTBEAT		EV_TIMER2	// (Unused) neighbour-update heartbeat
#define EV_NETWORKSEND		EV_TIMER3	// Send first network frame in queue
#define EV_NETWORKTIMEOUT	EV_TIMER4	// Handle network timeout
#define EV_DATALINKSEND		EV_TIMER5	// Send first datalink frame in queue
#define EV_DATALINKTIMEOUT	EV_TIMER6	// Handle datalink timeout
#define EV_NETWORKREADY		EV_TIMER7	// Network layer message received
#define	EV_DATALINKREADY	EV_TIMER8	// Datalink layer message received
#define	EV_WALKING			EV_TIMER9	// Random walk

const string broadcastaddr = "ff:ff:ff:ff:ff:ff";
const CnetTime datalink_acktime = 10000;	// Datalink timeout
const CnetTime network_acktime = 1000000;	// Network timeout

// Retrieve the string-form wireless NIC address of this node
string get_local_nicaddr() {
	string nic = "xx:xx:xx:xx:xx:xx";
	CNET_format_nicaddr((char *) nic.data(), linkinfo[1].nicaddr);
	return nic;
}

// src/seq/dst/type/body
unsigned int datalink_sequence_number;
struct DataLinkFrame {
	const char delim = '/';	// Delimiter character
	string src;	// Source
	unsigned int seq;	// Sequence number
	string dst;	// Destination
	string type;	// Frame type, "CONTENT" when transporting data for layer above
	string body;	// Body data
	DataLinkFrame(string dst, string type, string body) : src(get_local_nicaddr()), seq(datalink_sequence_number++), dst(dst), type(type), body(body) {}
	DataLinkFrame(string dst, string body) : DataLinkFrame(dst, "CONTENT", body) {}
	DataLinkFrame(string body) : DataLinkFrame(broadcastaddr, body) {}
	DataLinkFrame() : DataLinkFrame("") {}
	DataLinkFrame& operator=(const DataLinkFrame& rhs) {
		src = rhs.src;
		seq = rhs.seq;
		dst = rhs.dst;
		type = rhs.type;
		body = rhs.body;
		return *this;
	}
	// Serialize (human readable)
	string encode() {
		string frame;
		frame += src + delim;
		frame += to_string(seq) + delim;
		frame += dst + delim;
		frame += type + delim;
		frame += body;
		return frame;
	}
	// Deserialize
	void decode(string frame) {
		stringstream fs(frame);
		getline(fs, src, delim);
		fs >> seq;
		while (fs.good())
			if (fs.get() == delim) break;
		getline(fs, dst, delim);
		getline(fs, type, delim);
		body.clear();
		while (fs.good()) {
			string line;
			getline(fs, line);
			body += line;
		}
	}
};

// src/seq/dst/type/hops/body
unsigned int network_sequence_number;
struct NetworkFrame {
	const char delim = '/';	// Delimiter character
	string src;	// Source
	unsigned int seq;	// Sequence number
	string dst;	// Destination
	string type;	// Frame type, "CONTENT" when transporting data for layer above
	unsigned int hops;	// Node hops remaining
	string body;	// Body data
	NetworkFrame(string dst, string type, int hops, string body) : src(get_local_nicaddr()), seq(network_sequence_number++), dst(dst), type(type), hops(hops), body(body) {}
	NetworkFrame(string dst, string type, string body) : NetworkFrame(dst, type, 16, body) {}
	NetworkFrame(string dst, string body) : NetworkFrame(dst, "CONTENT", body) {}
	NetworkFrame(string body) : NetworkFrame(broadcastaddr, body) {}
	NetworkFrame() : NetworkFrame("") {}
	NetworkFrame& operator=(const NetworkFrame& rhs) {
		src = rhs.src;
		seq = rhs.seq;
		dst = rhs.dst;
		type = rhs.type;
		hops = rhs.hops;
		body = rhs.body;
		return *this;
	}
	// Serialize (human readable)
	string encode() {
		string frame;
		frame += src + delim;
		frame += to_string(seq) + delim;
		frame += dst + delim;
		frame += type + delim;
		frame += to_string(hops) + delim;
		frame += body;
		return frame;
	}
	// Deserialize
	void decode(string frame) {
		stringstream fs(frame);
		getline(fs, src, delim);
		fs >> seq;
		while (fs.good())
			if (fs.get() == delim) break;
		getline(fs, dst, delim);
		getline(fs, type, delim);
		fs >> hops;
		while (fs.good())
			if (fs.get() == delim) break;
		body.clear();
		while (fs.good()) {
			string line;
			getline(fs, line);
			body += line;
		}
	}
};

queue<string> * application_recv_queue;	// Received application-layer messages

set<string> * neighbours;	// NIC addresses of neighbours
map<string, set<string>> * routes;	// Neighbours that have a route to destination

queue<NetworkFrame> * network_send_queue;	// Network frames to be sent
map<int, NetworkFrame> * network_waiting;	// Sent frames awaiting ACK
queue<pair<CnetTime, int>> * network_sent_time;	// Transmission time (used for timeout)
map<int, string> * route_used;	// Route used (for removal on failure)
queue<string> * network_recv_queue;	// Received network layer messages

queue<DataLinkFrame> * datalink_send_queue;	// Datalink frames to be sent
map<int, DataLinkFrame> * datalink_waiting;	// Sent frames awaiting ACK
queue<pair<CnetTime, int>> * datalink_sent_time;	// Transmission time

// Helper functions to enqueue and schedule messages
void network_send_enqueue(NetworkFrame nfr) {
	network_send_queue->push(nfr);
	CNET_start_timer(EV_NETWORKSEND, 1, 0);
}

void datalink_send_enqueue(DataLinkFrame dfr) {
	datalink_send_queue->push(dfr);
	CNET_start_timer(EV_DATALINKSEND, 1, 0);
}

void network_recv_enqueue(string str) {
	network_recv_queue->push(str);
	CNET_start_timer(EV_DATALINKREADY, 1, 0);
}

void application_recv_enqueue(string str) {
	application_recv_queue->push(str);
	CNET_start_timer(EV_NETWORKREADY, 1, 0);
}

// Send first frame in network send queue
EVENT_HANDLER(network_send) {
	if (network_send_queue->empty()) return;

	NetworkFrame nfr = network_send_queue->front();
	network_send_queue->pop();
	if (nfr.dst != broadcastaddr && (routes->count(nfr.dst) == 0 || routes->at(nfr.dst).empty())) {	// No route to host
		network_send_queue->push(nfr);	// Requeue frame for retransmission
		CNET_start_timer(EV_NETWORKSEND, 10000, data);	// Schedule retransmission attempt
		if (neighbours->size() == 0) {	// If we are not currently aware of any neighbours, poll for them
			datalink_send_enqueue(DataLinkFrame(broadcastaddr, "SOUND_OFF", ""));
		} else {	// Perform a full reversal
			if (routes->count(nfr.dst) == 0)
				routes->emplace(nfr.dst, *neighbours);
			else
				routes->at(nfr.dst) = *neighbours;
			datalink_send_enqueue(DataLinkFrame(broadcastaddr, "FULL_REVERSAL", nfr.dst));
		}
	} else {	// Have route to host, transmit using route
		DataLinkFrame dfr;
		if (nfr.dst == broadcastaddr || routes->at(nfr.dst).count(nfr.dst))
			dfr = DataLinkFrame(nfr.dst, nfr.encode());
		else
			dfr = DataLinkFrame(*(routes->at(nfr.dst).begin()), nfr.encode());
		datalink_send_enqueue(dfr);
		if (nfr.src == get_local_nicaddr() && nfr.dst != broadcastaddr && nfr.type == "CONTENT") {	// Register for ACK if appropriate
			network_waiting->emplace(nfr.seq, nfr);
			route_used->emplace(nfr.seq, dfr.dst);
			network_sent_time->push(pair<CnetTime, int>(nodeinfo.time_in_usec, nfr.seq));
			CNET_start_timer(EV_NETWORKTIMEOUT, network_acktime, data);
		}
	}
}

// Handle network layer ack timeout
EVENT_HANDLER(network_timeout) {
	while (!(network_sent_time->empty()) && network_sent_time->front().first + network_acktime <= nodeinfo.time_in_usec) { // If a timer has expired
		int seq = network_sent_time->front().second;
		network_sent_time->pop();
		if (network_waiting->count(seq)) {
			NetworkFrame nfr = network_waiting->at(seq);
			network_waiting->erase(seq);
			routes->at(nfr.dst).erase(route_used->at(seq));	// Erase bad route
			route_used->erase(seq);
			application_recv_enqueue("NETWORK LAYER TIMEOUT: " + nfr.body);
		}
	}
}

// Send first frame in datalink send queue
EVENT_HANDLER(datalink_send) {
	if (datalink_send_queue->empty()) return;

	DataLinkFrame dfr = datalink_send_queue->front();
	datalink_send_queue->pop();
	if (dfr.dst != broadcastaddr && dfr.type == "CONTENT") {	// Register for ACK
		datalink_waiting->emplace(dfr.seq, dfr);
		datalink_sent_time->push(pair<CnetTime, int>(nodeinfo.time_in_usec, dfr.seq));
		CNET_start_timer(EV_DATALINKTIMEOUT, datalink_acktime, data);
	}
	string dfrstr = dfr.encode();
	size_t len = dfrstr.size();
	CHECK(CNET_write_physical_reliable(1, (void*) dfrstr.data(), &len));
	cout << "\t" << nodeinfo.nodenumber << " Transmitting: " << dfrstr << endl;
}

// Handle datalink timeout
EVENT_HANDLER(datalink_timeout) {
	while (!(datalink_sent_time->empty()) && datalink_sent_time->front().first + datalink_acktime <= nodeinfo.time_in_usec) {
		int seq = datalink_sent_time->front().second;
		datalink_sent_time->pop();
		if (datalink_waiting->count(seq)) {	// If failed to transmit, remove neighbour
			DataLinkFrame dfr = datalink_waiting->at(seq);
			datalink_waiting->erase(seq);
			neighbours->erase(dfr.dst);
			for (auto & kv : *routes) {
				kv.second.erase(dfr.dst);
			}
			NetworkFrame nfr;
			nfr.decode(dfr.body);
			network_send_enqueue(nfr);
		}
	}
}

// Received message from network layer, thin application layer just prints to STDOUT
EVENT_HANDLER(network_ready) {
	string message = application_recv_queue->front();
	application_recv_queue->pop();
	cout << nodeinfo.nodenumber << ": RECEIVED MESSAGE: " << message << endl;
}

// Received message from datalink layer, handle network layer frame
EVENT_HANDLER(datalink_ready) {
	string dfrstr = network_recv_queue->front();
	network_recv_queue->pop();
	NetworkFrame nfr;
	nfr.decode(dfrstr);
	--nfr.hops;	// Decrease hopcount
	if (nfr.dst == broadcastaddr || nfr.dst == get_local_nicaddr()) {	// If it's for us
		if (nfr.type == "ACK") {	// Received ACK
			cout << nodeinfo.nodenumber << ": RECEIVED ACK" << endl;
			unsigned int seq;
			istringstream(nfr.body) >> seq;
			network_waiting->erase(seq);
		} else if (nfr.type == "NACK") {	// Received NACK, notify application layer
			unsigned int seq;
			istringstream(nfr.body) >> seq;
			if (network_waiting->count(seq)) {
				nfr = network_waiting->at(seq);
				network_waiting->erase(seq);
				routes->at(nfr.dst).erase(route_used->at(seq));
				route_used->erase(seq);
				application_recv_enqueue("RECEIVED NACK: " + nfr.body);
			}
		} else if (nfr.type == "CONTENT") {	// Received CONTENT, pass up to application layer
			application_recv_enqueue(nfr.body);
			network_send_enqueue(NetworkFrame(nfr.src, "ACK", to_string(nfr.seq)));
		}
	} else {
		if (nfr.hops > 0) {	// Have hops left
			//if (nfr.type == "CONTENT")
			//	nfr.body += ", " + to_string(nodeinfo.nodenumber);
			network_send_enqueue(nfr);	// Forward message on
		} else {
			network_send_enqueue(NetworkFrame(nfr.src, "NACK", to_string(nfr.seq)));	// Drop message and send NACK
		}
	}
}

// Received physical layer message
EVENT_HANDLER(physical_ready) {
	string buf(linkinfo[1].mtu, 'X');
	size_t	len	= buf.size();
	int		link;

	CHECK(CNET_read_physical(&link, (void*) buf.data(), &len));
	buf.resize(len);

	DataLinkFrame dfr;
	dfr.decode(buf);

	// Add to neighbours, as it is in range
	neighbours->insert(dfr.src);
	if (routes->count(dfr.src) == 0)
		routes->emplace(dfr.src, set<string>());
	routes->at(dfr.src).insert(dfr.src);	// Add to own routes

	if (dfr.dst == broadcastaddr || dfr.dst == get_local_nicaddr()) {	// If addressed to us
		if (dfr.type == "ACK") {	// Handle ACK
			unsigned int seq;
			istringstream(dfr.body) >> seq;
			datalink_waiting->erase(seq);
		} else if (dfr.type == "FULL_REVERSAL") {
			if (routes->count(dfr.body))	// Remove route
				routes->at(dfr.body).erase(dfr.src);
			if (dfr.body != get_local_nicaddr() && (routes->count(dfr.body) == 0 || routes->at(dfr.body).empty())) {	// If we have no routes left, perform full reversal
				if (routes->count(dfr.body) == 0)
					routes->emplace(dfr.body, *neighbours);
				else
					routes->at(dfr.body) = *neighbours;
				datalink_send_enqueue(DataLinkFrame(broadcastaddr, "FULL_REVERSAL", dfr.body));
			} else if (dfr.body == get_local_nicaddr()) {	// If the reversal was to us, announce our presence to prevent oscillation
				datalink_send_enqueue(DataLinkFrame(broadcastaddr, "PRESENT", ""));
			}
		} else if (dfr.type == "SOUND_OFF") {	// Answer sound off to let neighbour know we exist
			datalink_send_enqueue(DataLinkFrame(broadcastaddr, "ONE_TWO", ""));
		} else if (dfr.type == "CONTENT") {	// Received content, pass up to layer above
			network_recv_enqueue(dfr.body);
			datalink_send_enqueue(DataLinkFrame(dfr.src, "ACK", to_string(dfr.seq)));
		}
	}
}

// (Unused) heartbeat to maintain neighbours
EVENT_HANDLER(heartbeat) {
	datalink_send_enqueue(DataLinkFrame(broadcastaddr, "PULSE", ""));
	CNET_start_timer(EV_HEARTBEAT, 10000000, data);
}

// Transmit a message periodically to test the network
int message_number = 0;
EVENT_HANDLER(testmsg) {
	network_send_enqueue(NetworkFrame("01:00:00:00:00:01", "This is test message " + to_string(++message_number) + " to 0 from " + to_string(nodeinfo.nodenumber)));
	CNET_start_timer(EV_TESTMSG, 1000000, data);
}

// Node has restarted
EVENT_HANDLER(reboot_node) {
	datalink_sequence_number = 0;
	network_sequence_number = 0;

	// Allocate datastructures
	application_recv_queue = new queue<string>;

	neighbours = new set<string>;
	routes = new map<string, set<string>>;

	network_send_queue = new queue<NetworkFrame>;
	network_waiting = new map<int, NetworkFrame>;
	network_sent_time = new queue<pair<CnetTime, int>>;
	route_used = new map<int, string>;
	network_recv_queue = new queue<string>;

	datalink_send_queue = new queue<DataLinkFrame>;
	datalink_waiting = new map<int, DataLinkFrame>;
	datalink_sent_time = new queue<pair<CnetTime, int>>;

	CNET_check_version(CNET_VERSION);
	CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

	// Register handlers
	CHECK(CNET_set_handler(EV_TESTMSG, testmsg, 0));
	CHECK(CNET_set_handler(EV_HEARTBEAT, heartbeat, 0));
	CHECK(CNET_set_handler(EV_NETWORKSEND, network_send, 0));
	CHECK(CNET_set_handler(EV_NETWORKTIMEOUT, network_timeout, 0));
	CHECK(CNET_set_handler(EV_DATALINKSEND, datalink_send, 0));
	CHECK(CNET_set_handler(EV_DATALINKTIMEOUT, datalink_timeout, 0));
	CHECK(CNET_set_handler(EV_NETWORKREADY, network_ready, 0));
	CHECK(CNET_set_handler(EV_DATALINKREADY, datalink_ready, 0));
	CHECK(CNET_set_handler(EV_PHYSICALREADY, physical_ready, 0));

	// Start heartbeat
	//CNET_start_timer(EV_HEARTBEAT, 10000000, 0);

	// Enable random walk on all nodes
	if(nodeinfo.nodetype == NT_MOBILE) {
		init_randomwalk(EV_WALKING);
		start_walking();
	}

	// Start test messages to 0
	if(nodeinfo.nodenumber == 2) {
		CNET_start_timer(EV_TESTMSG, 1000000, 0);
	}
}