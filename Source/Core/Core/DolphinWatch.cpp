
#include <thread>
#include <atomic>

#include "DolphinWatch.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/InputConfig.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "AudioCommon/AudioCommon.h"
#include "HW/ProcessorInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "BootManager.h"
//#include "Core/HW/DVDInterface.h"
#include "Core/Host.h"
#include "Core/ConfigManager.h"

//#include "Core/IPC_HLE/WII_IPC_HLE_WiiMote.h"
//#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb.h"

namespace DolphinWatch {

	static sf::TcpListener server;
	static std::vector<Client> clients;
	static char cbuf[1024];

	static std::thread thrMemory, thrRecv;
	static std::atomic<bool> running;
	static std::mutex client_mtx;

	static int hijacksWii[NUM_WIIMOTES];
	static int hijacksGC[NUM_GCPADS];

  static std::locale locale = std::locale::classic();

	WiimoteEmu::Wiimote* GetWiimote(int i_wiimote) {
		return ((WiimoteEmu::Wiimote*)Wiimote::GetConfig()->GetController(i_wiimote));
	}

	GCPad* GetGCPad(int i_pad) {
		return ((GCPad*)Pad::GetConfig()->GetController(i_pad));
	}

	void SendButtonsWii(int i_wiimote, u16 _buttons) {

		if (!Core::IsRunning()) {
			WARN_LOG(DOLPHINWATCH, "Trying to send wii button presses, but Core is not running.");
			return;
		}

		WiimoteEmu::Wiimote* wiimote = GetWiimote(i_wiimote);

		// disable reports from actual wiimote for a while, aka hijack for a while
    wiimote->SetReportingHijacked(true);
		hijacksWii[i_wiimote] = HIJACK_TIMEOUT;

		u8 data[23];
		memset(data, 0, sizeof(data));

		data[0] = 0xA1; // input (wiimote -> wii)
		data[1] = 0x37; // mode: Core Buttons and Accelerometer with 16 Extension Bytes
			            // because just core buttons does not work for some reason.
    ((WiimoteCommon::ButtonData*)(data + 2))->hex |= _buttons;

		// Only filling in button data breaks button inputs with the wii-cursor being active somehow

		// Fill accelerometer with stable position
		data[4] = 0x80; // neutral
		data[5] = 0x80; // neutral
		data[6] = 0x9a; // gravity

		// Fill the rest with some actual data I grabbed from the emulated wiimote.

		// 10 IR bytes, these encode the positions 534/291, 634/291, 524/291, 644/291 in a 1024x768 resolution.
		// which pretty much means: point roughly at the middle.
		// see http://wiibrew.org/wiki/Wiimote#Basic_Mode
		unsigned char stuff[16] = { 0x16, 0x23, 0x66, 0x7a, 0x23, 0x0c, 0x23, 0x66, 0x84, 0x23,
		// 6 extension bytes. The numbers, what do they mean? Does this break on other machines?
			0x0c, 0x4c, 0x1a, 0xfb, 0xe6, 0x43 };
		memcpy(data + 7, stuff, 16);

		std::stringstream ss;
    ss.imbue(locale);
		ss << "Sending wii buttons. wiimote: " << i_wiimote << ", buttons: 0x" << std::hex << _buttons;
		ss << ", IR: 0x" << std::hex;
		for (int i = 0; i < 10; i++) {
			ss << ((int)stuff[i]);
		}
		ss << ", extension: 0x" << std::hex;
		for (int i = 10; i < 16; i++) {
			ss << ((int)stuff[i]);
		}
		DEBUG_LOG(DOLPHINWATCH, "%s", ss.str().c_str());

		Core::Callback_WiimoteInterruptChannel(i_wiimote, wiimote->GetReportingChannel(), data, 23);

	}

	void SendButtonsGC(int i_pad, u16 _buttons, float stickX, float stickY, float substickX, float substickY) {
		if (!Core::IsRunning()) {
			// TODO error
			return;
		}

		if (stickX < -1 || stickX > 1 ||
			stickY < -1 || stickY > 1 ||
			substickX < -1 || substickX > 1 ||
			substickY < -1 || substickY > 1) {
			// TODO error
			return;
		}

		GCPad* pad = GetGCPad(i_pad);

		GCPadStatus status;
		status.err = PadError::PAD_ERR_NONE;
		// neutral joystick positions
		status.stickX = GCPadStatus::MAIN_STICK_CENTER_X + int(stickX * GCPadStatus::MAIN_STICK_RADIUS);
		status.stickY = GCPadStatus::MAIN_STICK_CENTER_Y + int(stickY * GCPadStatus::MAIN_STICK_RADIUS);
		status.substickX = GCPadStatus::C_STICK_CENTER_X + int(substickX * GCPadStatus::C_STICK_RADIUS);
		status.substickY = GCPadStatus::C_STICK_CENTER_Y + int(substickY * GCPadStatus::C_STICK_RADIUS);
		status.button = _buttons;

		pad->SetForcedInput(&status);
		hijacksGC[i_pad] = HIJACK_TIMEOUT;
	}

	void CheckHijacks() {
		if (!Core::IsRunning() || Core::GetState() != Core::State::Running) {
			return;
		}
		for (int i = 0; i < NUM_WIIMOTES; ++i) {
			if (hijacksWii[i] <= 0) continue;
			hijacksWii[i] -= WATCH_TIMEOUT;
			if (hijacksWii[i] <= 0) {
				hijacksWii[i] = 0;
				GetWiimote(i)->SetReportingHijacked(false);
			}
		}
		for (int i = 0; i < NUM_GCPADS; ++i) {
			if (hijacksGC[i] <= 0) continue;
			hijacksGC[i] -= WATCH_TIMEOUT;
			if (hijacksGC[i] <= 0) {
				hijacksGC[i] = 0;
				GCPadStatus status;
				status.err = PadError::PAD_ERR_NO_CONTROLLER;
				GetGCPad(i)->SetForcedInput(&status);
			}
		}
	}

	void Init(unsigned short port) {
		running = true;
		server.listen(port);

		memset(hijacksWii, 0, sizeof(hijacksWii));
		memset(hijacksGC, 0, sizeof(hijacksGC));

		// thread to monitor memory
		thrMemory = std::thread([]() {
			while (running) {
				{
          if (!Memory::IsInitialized()) continue;
          for (Client& client : clients) {
            // check subscriptions
            CheckSubs(client);
          }
				}
				Common::SleepCurrentThread(WATCH_TIMEOUT);
				CheckHijacks();
			}
		});

		// thread to handle incoming data.
		thrRecv = std::thread([]() {
			while (running) {
				Poll();
			}
		});
	}

	void Poll() {
		sf::SocketSelector selector;
		{
			std::lock_guard<std::mutex> locked(client_mtx);
			selector.add(server);
			for (Client& client : clients) {
				selector.add(*client.socket);
			}
		}
		bool timeout = !selector.wait(sf::seconds(1));
		std::lock_guard<std::mutex> locked(client_mtx);
		if (!timeout) {
			for (Client& client : clients) {
				if (selector.isReady(*client.socket)) {
					// poll incoming data from clients, then process
					PollClient(client);
				}
			}
			if (selector.isReady(server)) {
				// poll for new clients
				auto socket = std::make_shared<sf::TcpSocket>();
				if (server.accept(*socket) == sf::Socket::Done) {
					DEBUG_LOG(DOLPHINWATCH, "Client connected: %s:%d", socket->getRemoteAddress().toString().c_str(), socket->getRemotePort());
					Client client(socket);
					clients.push_back(client);
				}
			}
		}
		// remove disconnected clients
		auto new_end = remove_if(clients.begin(), clients.end(), [](Client& c) {
			return c.disconnected;
		});
		clients.erase(new_end, clients.end());
	}

	void Shutdown() {
		running = false;
		if (thrMemory.joinable()) thrMemory.join();
		if (thrRecv.joinable()) thrRecv.join();
		// socket closing is implicit for sfml library during destruction
	}

	void Process(Client& client, std::string& line) {
		// turn line into another stream
		std::istringstream parts(line);
    parts.imbue(locale);
		std::string cmd;

		DEBUG_LOG(DOLPHINWATCH, "Processing: %s", line.c_str());

		if (!(parts >> cmd)) {
			// no command, empty line, skip
			WARN_LOG(DOLPHINWATCH, "empty command line %s", line.c_str());
			return;
		}

		if (cmd == "WRITE") {

			if (!Memory::IsInitialized()) {
				ERROR_LOG(DOLPHINWATCH, "PowerPC memory not initialized, can't execute command: %s", line.c_str());
				return;
			}

			u32 mode, addr, val;

			if (!(parts >> mode >> addr >> val)) {
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			// Parsing OK
      switch (mode) {
      case 8:
        PowerPC::HostWrite_U8(val, addr);
        break;
      case 16:
        PowerPC::HostWrite_U16(val, addr);
        break;
      case 32:
        PowerPC::HostWrite_U32(val, addr);
        break;
      default:
        ERROR_LOG(DOLPHINWATCH, "Wrong mode for writing, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
      }
		}
		else if (cmd == "WRITE_MULTI") {

			if (!Memory::IsInitialized()) {
				ERROR_LOG(DOLPHINWATCH, "PowerPC memory not initialized, can't execute command: %s", line.c_str());
				return;
			}

			u32 addr, val;
			std::vector<u32> vals;

			if (!(parts >> addr >> val)) {
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			do {
				vals.push_back(val);
			} while ((parts >> val));

			// Parsing OK
      for (u32 v : vals) {
        PowerPC::HostWrite_U8(v, addr);
        addr++;
      }
		}
		else if (cmd == "READ") {

			if (!Memory::IsInitialized()) {
				ERROR_LOG(DOLPHINWATCH, "PowerPC memory not initialized, can't execute command: %s", line.c_str());
				return;
			}

			u32 mode, addr, val;

			if (!(parts >> mode >> addr)) {
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			// Parsing OK
      switch (mode) {
      case 8:
        val = PowerPC::HostRead_U8(addr);
        break;
      case 16:
        val = PowerPC::HostRead_U16(addr);
        break;
      case 32:
        val = PowerPC::HostRead_U32(addr);
        break;
      default:
        ERROR_LOG(DOLPHINWATCH, "Wrong mode for reading, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
        return;
      }

			std::ostringstream message;
      message.imbue(locale);
			message << "MEM " << addr << " " << val << std::endl;
			std::string messagestr = message.str();
			Send(*(client.socket), messagestr);

		}
		else if (cmd == "SUBSCRIBE") {

			u32 mode, addr;

			if (!(parts >> mode >> addr)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			// TODO handle overlapping subscribes etc. better. maybe by returning the mode again?

			for (Subscription& sub : client.subs) {
				if (sub.addr == addr) {
					return;
				}
			}

			if (mode == 8 || mode == 16 || mode == 32) {
				client.subs.push_back(Subscription(addr, mode));
			}
			else {
				ERROR_LOG(DOLPHINWATCH, "Wrong mode for subscribing, 8/16/32 required as 1st parameter. Command: %s", line.c_str());
				return;
			}

		}
		else if (cmd == "SUBSCRIBE_MULTI") {

			u32 size, addr;

			if (!(parts >> size >> addr)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			// TODO handle overlapping subscribes etc. better. maybe by returning the mode again?

			for (SubscriptionMulti& sub : client.subsMulti) {
				if (sub.addr == addr) {
					return;
				}
			}

			client.subsMulti.push_back(SubscriptionMulti(addr, size));

		}
		else if (cmd == "UNSUBSCRIBE") {

			u32 addr;

			if (!(parts >> addr)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			for (auto iter = client.subs.begin(); iter != client.subs.end(); ++iter) {
				if (iter->addr == addr) {
					client.subs.erase(iter);
					return;
				}
			}

		}
		else if (cmd == "UNSUBSCRIBE_MULTI") {

			u32 addr;

			if (!(parts >> addr)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			for (auto iter = client.subsMulti.begin(); iter != client.subsMulti.end(); ++iter) {
				if (iter->addr == addr) {
					client.subsMulti.erase(iter);
					return;
				}
			}

		}
		else if (cmd == "BUTTONSTATES_WII") {

			int i_wiimote;
			u16 states;

			if (!(parts >> i_wiimote >> states)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			if (i_wiimote >= NUM_WIIMOTES) {
				ERROR_LOG(DOLPHINWATCH, "Invalid wiimote number %d in: %s", i_wiimote, line.c_str());
				return;
			}

			SendButtonsWii(i_wiimote, states);

		}
		else if (cmd == "BUTTONSTATES_GC") {

			int i_pad;
			u16 states;
			float sx, sy, ssx, ssy;

			if (!(parts >> i_pad >> states >> sx >> sy >> ssx >> ssy)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			if (i_pad >= NUM_GCPADS) {
				ERROR_LOG(DOLPHINWATCH, "Invalid GCPad number %d in: %s", i_pad, line.c_str());
				return;
			}

			SendButtonsGC(i_pad, states, sx, sy, ssx, ssy);

		}
		else if (cmd == "PAUSE") {
			if (!Core::IsRunning()) {
				ERROR_LOG(DOLPHINWATCH, "Core not running, can't pause: %s", line.c_str());
				return;
			}
			Core::SetState(Core::State::Paused);
		}
		else if (cmd == "RESUME") {
			if (!Core::IsRunning()) {
				ERROR_LOG(DOLPHINWATCH, "Core not running, can't resume: %s", line.c_str());
				return;
			}
			Core::SetState(Core::State::Running);
		}
		else if (cmd == "RESET") {
			if (!Core::IsRunning()) {
				ERROR_LOG(DOLPHINWATCH, "Core not running, can't reset: %s", line.c_str());
				return;
			}
			ProcessorInterface::ResetButton_Tap();
		}
		else if (cmd == "SAVE") {

			if (!Core::IsRunning()) {
				ERROR_LOG(DOLPHINWATCH, "Core not running, can't save savestate: %s", line.c_str());
				return;
			}

			std::string file;
			getline(parts, file);
			file = StripSpaces(file);
			if (file.empty() || file.find_first_of("?\"<>|") != std::string::npos) {
				ERROR_LOG(DOLPHINWATCH, "Invalid filename for saving savestate: %s", line.c_str());
				return;
			}

			State::SaveAs(file);

		}
		else if (cmd == "LOAD") {

			if (!Core::IsRunning()) {
				ERROR_LOG(DOLPHINWATCH, "Core not running, can't load savestate: %s", line.c_str());
				SendFeedback(client, false);
				return;
			}

			std::string file;
			getline(parts, file);
			file = StripSpaces(file);
			if (file.empty() || file.find_first_of("?\"<>|") != std::string::npos) {
				ERROR_LOG(DOLPHINWATCH, "Invalid filename for loading savestate: %s", line.c_str());
				SendFeedback(client, false);
				return;
			}

			bool success = State::LoadAs(file);
			if (!success) {
				ERROR_LOG(DOLPHINWATCH, "Could not load savestate: %s", file.c_str());
			}
			SendFeedback(client, success);

		}
		else if (cmd == "VOLUME") {

			int v;

			if (!(parts >> v)) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
				return;
			}

			if (v < 0 || v > 100) {
				// no valid parameters, skip
				ERROR_LOG(DOLPHINWATCH, "Invalid volume, must be between 0 and 100: %s", line.c_str());
				return;
			}

			SetVolume(v);

		}
    else if (cmd == "SPEED") {
      float speed;

      if (!(parts >> speed)) {
        // no valid parameters, skip
        ERROR_LOG(DOLPHINWATCH, "Invalid command line: %s", line.c_str());
        return;
      }

      SConfig::GetInstance().m_EmulationSpeed = speed;
    }
		else if (cmd == "STOP") {
			//BootManager::Stop();
			Core::Stop();
		}
		else if (cmd == "INSERT") {
			std::string file;
			getline(parts, file);
			file = StripSpaces(file);
			if (file.empty() || file.find_first_of("?\"<>|") != std::string::npos) {
				ERROR_LOG(DOLPHINWATCH, "Invalid filename for iso/discfile to insert: %s", line.c_str());
				//sendFeedback(client, false);
				return;
			}

			// Not working yet, see https://bugs.dolphin-emu.org/issues/9019
			//DVDInterface::ChangeDisc(file);
			//ProcessorInterface::ResetButton_Tap();
		}
		else {
			ERROR_LOG(DOLPHINWATCH, "Unknown command: %s", cmd.c_str());
		}
	}

	void SendFeedback(Client& client, bool success) {
		std::ostringstream msg;
    msg.imbue(locale);
		if (success) msg << "SUCCESS";
		else msg << "FAIL";
		msg << std::endl;
		std::string messagestr = msg.str();
		Send(*(client.socket), messagestr);
	}

	void CheckSubs(Client& client) {
		if (!Memory::IsInitialized()) return;
		for (Subscription& sub : client.subs) {
			u32 val = 0;
            if (sub.mode == 8) val = PowerPC::HostRead_U8(sub.addr);
            else if (sub.mode == 16) val = PowerPC::HostRead_U16(sub.addr);
            else if (sub.mode == 32) val = PowerPC::HostRead_U32(sub.addr);
			if (val != sub.prev) {
				sub.prev = val;
				std::ostringstream message;
        message.imbue(locale);
				message << "MEM " << sub.addr << " " << val << std::endl;
				std::string messagestr = message.str();
				Send(*(client.socket), messagestr);
			}
		}
		for (SubscriptionMulti& sub : client.subsMulti) {
			std::vector<u32> val(sub.size, 0);
            for (u32 i = 0; i < sub.size; ++i) {
                val.at(i) = PowerPC::HostRead_U8(sub.addr + i);
            }
			if (val != sub.prev) {
				sub.prev = val;
				std::ostringstream message;
        message.imbue(locale);
				message << "MEM_MULTI " << sub.addr << " ";
				for (size_t i = 0; i < val.size(); ++i) {
					if (i != 0) message << " ";
					message << val.at(i);
				}
				message << std::endl;
				std::string messagestr = message.str();
				Send(*(client.socket), messagestr);
			}
		}
	}

	void PollClient(Client& client) {
		// clean the client's buffer.
		// By default a stringbuffer keeps already read data.
		// a deque would do what we want, by not keeping that data, but then we would not have
		// access to nice stream-features like <</>> operators and getline(), so we do this manually.

		std::string s;
		client.buf.clear(); // reset eol flag
		getline(client.buf, s, '\0'); // read everything
		client.buf.clear(); // reset eol flag again
		client.buf.str(""); // empty stringstream
		client.buf << s; // insert rest at beginning again

		size_t received = 0;
		auto status = client.socket->receive(cbuf, sizeof(cbuf) - 1, received);
		if ((status == sf::Socket::Disconnected) || (status == sf::Socket::Error)) {
			DEBUG_LOG(DOLPHINWATCH, "Client disconnected: %s:%d", client.socket->getRemoteAddress().toString().c_str(), client.socket->getRemotePort());
			client.disconnected = true;
		}
		else if (status == sf::Socket::Done) {
			// add nullterminator, then add to client's buffer
			cbuf[received] = '\0';
			client.buf << cbuf;

			// process the client's buffer
			size_t posg = 0;
			while (getline(client.buf, s)) {
				if (client.buf.eof()) {
					client.buf.clear();
					client.buf.seekg(posg);
					break;
				}
				posg = client.buf.tellg();

				// Might contain semicolons to further split several commands.
				// Doing that ensures that those commands are executed at once / in the same emulated frame.
        // TODO not guaranteed in the same emulation frame, since we don't pause the emulation and it's concurrent
				std::string s2;
				std::istringstream subcmds(s);
        subcmds.imbue(locale);
				while (getline(subcmds, s2, ';')) {
                  if (!s2.empty()) Process(client, s2);
				}
			}
		}
	}

	void Send(sf::TcpSocket& socket, std::string& message) {
		DEBUG_LOG(DOLPHINWATCH, "Sending: %s", message.c_str());
		socket.send(message.c_str(), message.size());
	}

	void SetVolume(int v) {
		SConfig::GetInstance().m_Volume = v;
		AudioCommon::UpdateSoundStream();
	}

}
