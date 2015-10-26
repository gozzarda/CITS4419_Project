#include <iostream>
#include <string>
#include <sstream>
#include <cstdint>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include "project.h"

using namespace std;

#define EV_TIMEOUT EV_TIMER4
#define EV_TESTMSG EV_TIMER5
#define EV_TRANSMIT	EV_TIMER6
#define EV_NETWORKREADY	EV_TIMER7
#define	EV_DATALINKREADY	EV_TIMER8
#define	EV_WALKING	EV_TIMER9

const string broadnicaddr = "ff:ff:ff:ff:ff:ff";
const CnetTime ack_timeout = 1000000;

string get_local_nicaddr() {
	string nic = "xx:xx:xx:xx:xx:xx";
	CNET_format_nicaddr((char *) nic.data(), linkinfo[1].nicaddr);
	return nic;
}

// src/dst/body
struct DataFrame {
	const char delim = '/';
	string src = broadnicaddr;
	string dst = broadnicaddr;
	string body;
	DataFrame() {}
	DataFrame(string body) : src(get_local_nicaddr()), body(body) {}
	DataFrame(string dst, string body) : src(get_local_nicaddr()), dst(dst), body(body) {}
	DataFrame& operator=(const DataFrame& rhs) {
		src = rhs.src;
		dst = rhs.dst;
		body = rhs.body;
		return *this;
	}
	string encode() {
		string frame;
		frame += src + delim;
		frame += dst + delim;
		frame += body;
		if (frame.size() > linkinfo[1].mtu) throw "DataFrame exceeds MTU";
		return frame;
	}
	void decode(string frame) {
		stringstream fs(frame);
		getline(fs, src, delim);
		getline(fs, dst, delim);
		body.clear();
		while (fs.good()) {
			string line;
			getline(fs, line);
			body += line;
		}
	}
};

// src/dst/type/hops/body
struct NetFrame {
	const char delim = '/';
	string src = broadnicaddr;
	int seq = 0;
	string dst = broadnicaddr;
	string type = "MESSAGE";
	int hops = 16;
	string body;
	NetFrame() {}
	NetFrame(string body) : src(get_local_nicaddr()), seq(seq), body(body) {}
	NetFrame(string dst, string body) : src(get_local_nicaddr()), seq(seq), dst(dst), body(body) {}
	NetFrame(string dst, string type, string body) : src(get_local_nicaddr()), seq(seq), dst(dst), type(type), body(body) {}
	NetFrame& operator=(const NetFrame& rhs) {
		src = rhs.src;
		seq = rhs.seq;
		dst = rhs.dst;
		type = rhs.type;
		hops = rhs.hops;
		body = rhs.body;
		return *this;
	}
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

int sequence_number;
set<string> * neighbours;
map<string, set<string>> * routes;
queue<NetFrame> * transmit_queue;
map<int, NetFrame> * sent_messages;
queue<pair<CnetTime, int>> * sent_time;
map<int, string> * route_used;
queue<string> * datalink_queue;
queue<string> * network_queue;

EVENT_HANDLER(transmit) {
	if (transmit_queue->empty()) return;

	NetFrame msg = transmit_queue->front();
	transmit_queue->pop();
	DataFrame frame;
	if (msg.dst != broadnicaddr && (routes->count(msg.dst) == 0 || routes->at(msg.dst).empty())) {
		transmit_queue->push(msg);
		if (neighbours->size() == 0) {
			NetFrame nfr(broadnicaddr, "SOUND_OFF", "");
			frame = DataFrame(nfr.encode());
			CNET_start_timer(EV_TRANSMIT, 1000, data);	// Schedule retransmission attempt
		} else {
			if (routes->count(msg.dst) == 0)
				routes->emplace(msg.dst, *neighbours);
			else
				routes->at(msg.dst) = *neighbours;
			NetFrame nfr(broadnicaddr, "FULL_REVERSAL", msg.dst);
			frame = DataFrame(nfr.encode());
			CNET_start_timer(EV_TRANSMIT, 10000, data);	// Schedule retransmission attempt
		}
	} else {
		if (msg.dst == broadnicaddr || routes->at(msg.dst).count(msg.dst))
			frame = DataFrame(msg.dst, msg.encode());
		else
			frame = DataFrame(*(routes->at(msg.dst).begin()), msg.encode());
		if (msg.src == get_local_nicaddr() && msg.type == "MESSAGE") {
			sent_messages->emplace(msg.seq, msg);
			route_used->emplace(msg.seq, frame.dst);
			sent_time->push(pair<CnetTime, int>(nodeinfo.time_in_usec, msg.seq));
		}
		CNET_start_timer(EV_TIMEOUT, ack_timeout, data);
	}
	string framestr = frame.encode();
	size_t len = framestr.size();
	CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
}

EVENT_HANDLER(timeout) {
	while (!(sent_time->empty()) && sent_time->front().first + ack_timeout <= nodeinfo.time_in_usec) {
		int seq = sent_time->front().second;
		sent_time->pop();
		if (sent_messages->count(seq)) {
			NetFrame msg = sent_messages->at(seq);
			sent_messages->erase(seq);
			transmit_queue->push(msg);
			CNET_start_timer(EV_TRANSMIT, 1, data);
			routes->at(msg.dst).erase(route_used->at(seq));
			route_used->erase(seq);
		}
	}
}

EVENT_HANDLER(network_ready) {
	string message = network_queue->front();
	network_queue->pop();
	cout << nodeinfo.nodenumber << ": RECEIVED MESSAGE: " << message << endl;
}

EVENT_HANDLER(datalink_ready) {
	string dframestr = datalink_queue->front();
	datalink_queue->pop();
	NetFrame nfr;
	nfr.decode(dframestr);
	--nfr.hops;
	if (nfr.dst == broadnicaddr || nfr.dst == get_local_nicaddr()) {
		if (nfr.type == "ACK") {
			cout << nodeinfo.nodenumber << ": RECEIVED ACK" << endl;
			sent_messages->erase(nfr.seq);
		} else if (nfr.type == "NACK") {
			if (sent_messages->count(nfr.seq)) {
				routes->at(sent_messages->at(nfr.seq).dst).erase(route_used->at(nfr.seq));
				route_used->erase(nfr.seq);
				NetFrame msg = sent_messages->at(nfr.seq);
				msg.seq = sequence_number++;
				transmit_queue->push(msg);
				sent_messages->erase(nfr.seq);
			}
		} else if (nfr.type == "FULL_REVERSAL") {
			if (routes->count(nfr.body))
				routes->at(nfr.body).erase(nfr.src);
			if (nfr.body != get_local_nicaddr() && (routes->count(nfr.body) == 0 || routes->at(nfr.body).empty())) {
				NetFrame response = NetFrame(broadnicaddr, "FULL_REVERSAL", nfr.body);
				DataFrame frame(response.encode());
				string framestr = frame.encode();
				size_t len = framestr.size();
				CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
			}
		} else if (nfr.type == "SOUND_OFF") {
			NetFrame response = NetFrame(broadnicaddr, "ONE_TWO", "");
			DataFrame frame(response.encode());
			string framestr = frame.encode();
			size_t len = framestr.size();
			CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
		} else if (nfr.type == "MESSAGE") {
			network_queue->push(nfr.body);
			CNET_start_timer(EV_NETWORKREADY, 1, data);
			NetFrame ack(nfr.src, "ACK", "");
			ack.seq = nfr.seq;
			transmit_queue->push(ack);
			CNET_start_timer(EV_TRANSMIT, 1, data);
		}
	} else {
		if (nfr.hops > 0) {
			nfr.body += ", " + to_string(nodeinfo.nodenumber);
			transmit_queue->push(nfr);
			CNET_start_timer(EV_TRANSMIT, 1, data);
		} else {
			NetFrame nack(nfr.src, "NACK", "");
			nack.seq = nfr.seq;
			DataFrame frame(nack.encode());
			string framestr = frame.encode();
			size_t len = framestr.size();
			CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
		}
	}
}

EVENT_HANDLER(physical_ready) {
	string buf(linkinfo[1].mtu, 'X');
	size_t	len	= buf.size();
	int		link;

	CHECK(CNET_read_physical(&link, (void*) buf.data(), &len));
	buf.resize(len);

	DataFrame dfr;
	dfr.decode(buf);

	neighbours->insert(dfr.src);
	if (routes->count(dfr.src) == 0)
		routes->emplace(dfr.src, set<string>());
	routes->at(dfr.src).insert(dfr.src);

	if (dfr.dst == broadnicaddr || dfr.dst == get_local_nicaddr()) {
		datalink_queue->push(dfr.body);
		CNET_start_timer(EV_DATALINKREADY, 1, data);
	}
}

int message_number = 0;
EVENT_HANDLER(testmsg) {
	NetFrame msg("01:00:00:00:00:01", "This is test message " + to_string(++message_number) + " from 2 to 0");
	msg.seq = sequence_number++;
	transmit_queue->push(msg);
	CNET_start_timer(EV_TRANSMIT, 1, data);
	CNET_start_timer(EV_TESTMSG, 1000000, data);
}

EVENT_HANDLER(reboot_node) {
	sequence_number = 1;
	neighbours = new set<string>;
	routes = new map<string, set<string>>;
	transmit_queue = new queue<NetFrame>;
	sent_messages = new map<int, NetFrame>;
	sent_time = new queue<pair<CnetTime, int>>;
	route_used = new map<int, string>;
	datalink_queue = new queue<string>;
	network_queue = new queue<string>;

	CNET_check_version(CNET_VERSION);
	CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

	CHECK(CNET_set_handler(EV_PHYSICALREADY, physical_ready, 0));
	CHECK(CNET_set_handler(EV_DATALINKREADY, datalink_ready, 0));
	CHECK(CNET_set_handler(EV_NETWORKREADY, network_ready, 0));
	CHECK(CNET_set_handler(EV_TRANSMIT, transmit, 0));
	CHECK(CNET_set_handler(EV_TIMEOUT, timeout, 0));
	CHECK(CNET_set_handler(EV_TESTMSG, testmsg, 0));

	if(nodeinfo.nodetype == NT_MOBILE && nodeinfo.nodenumber == 2) {
		init_randomwalk(EV_WALKING);
		start_walking();
	}
	if(nodeinfo.nodenumber == 2) {
		CNET_start_timer(EV_TESTMSG, 1000000, 0);
	}
}