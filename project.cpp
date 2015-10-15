#include <iostream>
#include <string>
#include <sstream>
#include <random>
#include <cstdint>
#include <tuple>
#include "project.h"

using namespace std;

#define	EV_TRANSMIT	EV_TIMER8
#define	EV_WALKING	EV_TIMER9

bool set = false;
int slot_time;

// crc:src:dst:body
struct DataFrame {
	const char delim = ':';
	int src = -1;
	int dst = -1;
	string body;
	DataFrame() {}
	DataFrame(string body) : src(nodeinfo.nodenumber), body(body) {}
	DataFrame(int dst, string body) : src(nodeinfo.nodenumber), dst(dst), body(body) {}
	string encode() {
		string frame;
		frame += to_string(src) + delim;
		frame += to_string(dst) + delim;
		frame += body;
		uint32_t crc = CNET_crc32((unsigned char *) frame.data(), frame.size());
		frame = to_string(crc) + delim + frame;
		if (frame.size() > linkinfo[1].mtu) throw "DataFrame exceeds MTU";
		return frame;
	}
	bool decode(string frame) {
		stringstream fs(frame);
		uint32_t crc;
		fs >> crc;
		if (fs.get() != delim) return false;
		fs >> src;
		if (fs.get() != delim) return false;
		fs >> dst;
		if (fs.get() != delim) return false;
		body.clear();
		while (fs.good()) {
			string line;
			getline(fs, line);
			body += line;
		}
		if (frame != encode()) return false;
		return true;
	}
};

EVENT_HANDLER(transmit) {
	/*if (CNET_carrier_sense(1) == 1) {
		CNET_start_timer(EV_TRANSMIT, slot_time, data);
		return;
	}*/
	cout << nodeinfo.nodenumber << ": transmitting" << endl;
	DataFrame frame("THIS IS A TEST FROM " + to_string(nodeinfo.nodenumber));
	string framestr = frame.encode();
	size_t len = framestr.size();
	CHECK(CNET_write_physical_reliable(1, (void*) framestr.data(), &len));
	set = false;
}

EVENT_HANDLER(receive) {
	string buf(linkinfo[1].mtu, 'X');
	size_t	len	= buf.size();
	int		link;

	CHECK(CNET_read_physical(&link, (void*) buf.data(), &len));
	buf.resize(len);
	DataFrame frame;
	cout << "\t\t" << nodeinfo.nodenumber << ": ";
	if (frame.decode(buf)) {
		if (frame.dst == -1 || frame.dst == nodeinfo.nodenumber) {
			cout << "Received message from " << frame.src << ", message reads: " << frame.body;
		} else {
			cout << "Sensed message from " << frame.src;
		}
	} else {
		cout << "Sensed corrupted frame";
	}
	cout << endl;

	if (!set) {
		CNET_start_timer(EV_TRANSMIT, slot_time * ((1000000 + slot_time - 1) / slot_time), data);
		set = true;
	}
}

const int backoff_limit = 4;
CnetTime last_force = -1000000;
EVENT_HANDLER(collision) {
	if (nodeinfo.time_in_usec - last_force >= slot_time) {
		last_force = nodeinfo.time_in_usec;
		DataFrame frame("FORCE COLLISION");
		string framestr = frame.encode();
		size_t len = framestr.size();
		CNET_write_physical(1, (void*) framestr.data(), &len);
	}

	if (!set) {
		int backoff = 1 << (CNET_rand() % (backoff_limit + 1));
		CNET_start_timer(EV_TRANSMIT, slot_time * backoff, data);
		set = true;
		cout << nodeinfo.nodenumber << " BACKOFF: " << backoff << endl;
	}
}

EVENT_HANDLER(reboot_node) {
	slot_time = (linkinfo[1].mtu * 8 * 1000000 + linkinfo[1].bandwidth - 1) / linkinfo[1].bandwidth;
	slot_time += slot_time / 16;

	CNET_check_version(CNET_VERSION);
	CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

	CHECK(CNET_set_handler(EV_PHYSICALREADY, receive, 0));
	CHECK(CNET_set_handler(EV_FRAMECOLLISION, collision, 0));

	if(nodeinfo.nodetype == NT_MOBILE) {
		CHECK(CNET_set_handler(EV_TRANSMIT, transmit, 0));
		//CNET_start_timer(EV_TRANSMIT, TALK_FREQUENCY, 0);

		init_randomwalk(EV_WALKING);
		start_walking();
	}
	if(nodeinfo.nodenumber == 0) {
		CNET_start_timer(EV_TRANSMIT, 1, 0);
	}
}