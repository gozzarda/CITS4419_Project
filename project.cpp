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

#define EV_TESTMSG	EV_TIMER6
#define EV_TIMEOUT	EV_TIMER7
#define	EV_TRANSMIT	EV_TIMER8
#define	EV_WALKING	EV_TIMER9

const string broadnicaddr = "ff:ff:ff:ff:ff:ff";
const CnetTime acktime = 1000000;

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
		char c;
		src.clear();
		while (fs.good() && (c = fs.get()) != delim) src += c;
		dst.clear();
		while (fs.good() && (c = fs.get()) != delim) dst += c;
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
	int seq;
	string dst = broadnicaddr;
	string type = "MESSAGE";
	int hops = 16;
	string body;
	NetFrame() {}
	NetFrame(int seq, string body) : src(get_local_nicaddr()), seq(seq), body(body) {}
	NetFrame(int seq, string dst, string body) : src(get_local_nicaddr()), seq(seq), dst(dst), body(body) {}
	NetFrame(int seq, string dst, string type, string body) : src(get_local_nicaddr()), seq(seq), dst(dst), type(type), body(body) {}
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
		char c;
		src.clear();
		while (fs.good() && (c = fs.get()) != delim) src += c;
		fs >> seq;
		while (fs.good())
			if (fs.get() == delim) break;
		dst.clear();
		while (fs.good() && (c = fs.get()) != delim) dst += c;
		type.clear();
		while (fs.good() && (c = fs.get()) != delim) type += c;
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
queue<NetFrame> * msgstosend;
map<int, NetFrame> * sentmsgs;
queue<pair<CnetTime, int>> * sendtime;
map<string, set<string>> * routes;
map<int, string> * routeused;

EVENT_HANDLER(transmit) {
	if (msgstosend->empty()) return;

	if (nodeinfo.nodenumber == 2) {
		cerr << "2: routes = " << endl;
		for (auto & kv : *routes) {
			cerr << "\t" << kv.first << "  : ";
			for (auto el : kv.second) {
				cerr << " " << el;
			}
			cerr << endl;
		}
	}

	NetFrame msg = msgstosend->front();
	DataFrame frame;
	if (msg.dst != broadnicaddr && (routes->count(msg.dst) == 0 || routes->at(msg.dst).empty())) {
		NetFrame nfr(sequence_number++, broadnicaddr, "FULL_REVERSAL", msg.dst);
		frame = DataFrame(nfr.encode());
	} else {
		msgstosend->pop();
		msg.seq = sequence_number++;
		if (msg.dst == broadnicaddr || routes->at(msg.dst).count(msg.dst))
			frame = DataFrame(msg.dst, msg.encode());
		else
			frame = DataFrame(*(routes->at(msg.dst).begin()), msg.encode());
		sentmsgs->emplace(msg.seq, msg);
		routeused->emplace(msg.seq, frame.dst);
		sendtime->push(pair<CnetTime, int>(nodeinfo.time_in_usec, msg.seq));
		CNET_start_timer(EV_TIMEOUT, acktime, data);
	}
	string framestr = frame.encode();
	size_t len = framestr.size();
	CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
	CNET_start_timer(EV_TRANSMIT, 1000, data);
}

EVENT_HANDLER(timeout) {
	while (!(sendtime->empty()) && sendtime->front().first + acktime <= nodeinfo.time_in_usec) {
		if (sentmsgs->count(sendtime->front().second)) {
			msgstosend->push(sentmsgs->at(sendtime->front().second));
			routes->at(sentmsgs->at(sendtime->front().second).dst).erase(routeused->at(sendtime->front().second));
			sentmsgs->erase(sendtime->front().second);
			routeused->erase(sendtime->front().second);
			CNET_start_timer(EV_TRANSMIT, 1, data);
		}
		sendtime->pop();
	}
}

EVENT_HANDLER(receive) {
	string buf(linkinfo[1].mtu, 'X');
	size_t	len	= buf.size();
	int		link;

	CHECK(CNET_read_physical(&link, (void*) buf.data(), &len));
	buf.resize(len);

	DataFrame dfr;
	dfr.decode(buf);

	if (dfr.dst == broadnicaddr || dfr.dst == get_local_nicaddr()) {
		NetFrame nfr;
		nfr.decode(dfr.body);
		--nfr.hops;
		if (nfr.dst == broadnicaddr || nfr.dst == get_local_nicaddr()) {
			if (nfr.type == "ACK") {
				sentmsgs->erase(nfr.seq);
			} else if (nfr.type == "NACK") {
				if (sentmsgs->count(nfr.seq)) {
					routes->at(sentmsgs->at(nfr.seq).dst).erase(routeused->at(nfr.seq));
					routeused->erase(nfr.seq);
					msgstosend->push(sentmsgs->at(nfr.seq));
					sentmsgs->erase(nfr.seq);
				}
			} else if (nfr.type == "FULL_REVERSAL") {
				if (routes->count(nfr.body)) routes->at(nfr.body).erase(nfr.src);
				NetFrame response;
				//cerr << nodeinfo.nodenumber << ": Received FULL_REVERSAL (" << nfr.body << ") from " << nfr.src << ". Response: ";
				if ((routes->count(nfr.body) && !(routes->at(nfr.body).empty())) || nfr.body == get_local_nicaddr()) {
					response = NetFrame(0, broadnicaddr, "ROUTE_ANNOUNCE", nfr.body);
					//cerr << "ROUTE_ANNOUNCE" << endl;
				} else {
					response = NetFrame(0, broadnicaddr, "FULL_REVERSAL", nfr.body);
					//cerr << "FULL_REVERSAL" << endl;
				}
				DataFrame frame(response.encode());
				string framestr = frame.encode();
				size_t len = framestr.size();
				CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
			} else if (nfr.type == "ROUTE_ANNOUNCE") {
				if (routes->count(nfr.body) == 0) routes->emplace(nfr.body, set<string>());
				routes->at(nfr.body).insert(nfr.src);
			} else if (nfr.type == "MESSAGE") {
				cout << "\t" << nodeinfo.nodenumber << ": Received message from " << nfr.src << ", message reads: " << nfr.body << endl;
				NetFrame ack(nfr.seq, nfr.src, "ACK", "");
				DataFrame frame(ack.encode());
				string framestr = frame.encode();
				size_t len = framestr.size();
				CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
			}
		} else {
			if (nfr.hops > 0) {
				msgstosend->push(nfr);
				CNET_start_timer(EV_TRANSMIT, 1000000, data);
			} else {
				NetFrame nack(nfr.seq, nfr.src, "NACK", "");
				DataFrame frame(nack.encode());
				string framestr = frame.encode();
				size_t len = framestr.size();
				CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
			}
		}
	}
}

EVENT_HANDLER(testmsg) {
	static int msg_num = 0;
	msgstosend->push(NetFrame(0, "01:00:00:00:00:01", "This is test message " + to_string(msg_num++) + " from node " + to_string(nodeinfo.nodenumber)));
	CNET_start_timer(EV_TRANSMIT, 1, data);
	CNET_start_timer(EV_TESTMSG, 100000, data);
}

EVENT_HANDLER(reboot_node) {
	msgstosend = new queue<NetFrame>();
	sentmsgs = new map<int, NetFrame>();
	sendtime = new queue<pair<CnetTime, int>>;
	routes = new map<string, set<string>>;
	routeused = new map<int, string>();

	CNET_check_version(CNET_VERSION);
	CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

	sequence_number = 0;
	CHECK(CNET_set_handler(EV_PHYSICALREADY, receive, 0));
	CHECK(CNET_set_handler(EV_TRANSMIT, transmit, 0));
	CHECK(CNET_set_handler(EV_TIMEOUT, timeout, 0));
	CHECK(CNET_set_handler(EV_TESTMSG, testmsg, 0));

	if(nodeinfo.nodetype == NT_MOBILE && nodeinfo.nodenumber == 2) {
		init_randomwalk(EV_WALKING);
		start_walking();
	}
	if(nodeinfo.nodenumber == 2) {
		CNET_start_timer(EV_TESTMSG, 1, 0);
	}
}