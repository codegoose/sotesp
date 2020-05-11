#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <rang.hpp>
#include <json.hpp>
#include <fmt/format.h>
#include <curl/curl.h>
#include <curl/easy.h>

#include "cg.h"

#include <winsock2.h>
#include <windows.h>
#include <winuser.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>
#include <memoryapi.h>
#include <psapi.h>
#include <dwmapi.h>

#define GLEW_STATIC
#include <gl/glew.h>
#include <glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <glfw3native.h>

using fmt::format;

auto codegoose = R"(
           _                         
 ___ ___ _| |___ ___ ___ ___ ___ ___ 
|  _| . | . | -_| . | . | . |_ -| -_|
|___|___|___|___|_  |___|___|___|___|
                |___|                

)";

/*
	UTILITY
*/

namespace cg {

	struct hex_to_dec_table {
		long long tab[128];
		constexpr hex_to_dec_table() : tab { } {
			tab['0'] = 0;
			tab['1'] = 1;
			tab['2'] = 2;
			tab['3'] = 3;
			tab['4'] = 4;
			tab['5'] = 5;
			tab['6'] = 6;
			tab['7'] = 7;
			tab['8'] = 8;
			tab['9'] = 9;
			tab['a'] = 10;
			tab['A'] = 10;
			tab['b'] = 11;
			tab['B'] = 11;
			tab['c'] = 12;
			tab['C'] = 12;
			tab['d'] = 13;
			tab['D'] = 13;
			tab['e'] = 14;
			tab['E'] = 14;
			tab['f'] = 15;
			tab['F'] = 15;
		}

		constexpr long long operator[](const char &idx) const {
			return tab[(std::size_t) idx];
		} 
	} constexpr hex_to_dec_table;
}

void cg::show_dumb_banner() {
	std::cout << rang::fg::magenta << codegoose << rang::fg::reset;
}

constexpr char cg::hex_to_dec(const char &hex) {
	return hex_to_dec_table[static_cast<size_t>(hex)];
}

constexpr char cg::double_hex_to_dec(const char *hex) {
	return static_cast<char>(hex_to_dec(hex[0]) * 16) + hex_to_dec(hex[1]);
}

std::vector<std::string> cg::split_tokens(const std::string_view &token, const std::string_view &source) {
	std::vector<std::string> tokens;
	size_t last_index = 0;
	for (;;) {
		auto index = source.find(token, last_index);
		if (index == std::string::npos) {
			if (last_index < source.size()) tokens.push_back(std::move(static_cast<std::string>(source.substr(last_index))));
			break;
		}
		tokens.push_back(std::move(static_cast<std::string>(source.substr(last_index, index - last_index))));
		last_index = index + token.size();
	}
	return std::move(tokens);
}

/*
	CURL
*/

namespace cg::curl {

	struct lifetime_manager {

		CURL * const handle;
		std::vector<char> receive_buffer;

		lifetime_manager() : handle(curl_easy_init()) {
			if (!handle) return;
			curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(handle, CURLOPT_USERAGENT, "codegoose");
		}

		~lifetime_manager() {
			curl_easy_cleanup(handle);
		}

	} thread_local thread_local_instance;

	size_t receive_incoming_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
		size_t incoming_size = size * nmemb;
		size_t old_buffer_size = thread_local_instance.receive_buffer.size();
		thread_local_instance.receive_buffer.resize(old_buffer_size + incoming_size);
		memcpy(&thread_local_instance.receive_buffer[old_buffer_size], ptr, incoming_size);
		return incoming_size;
	}
}

std::optional<std::vector<char>> cg::curl::get(const std::string_view &url) {
	if (!thread_local_instance.handle) return std::nullopt;
	curl_easy_setopt(thread_local_instance.handle, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_setopt(thread_local_instance.handle, CURLOPT_URL, url.data());
	curl_easy_setopt(thread_local_instance.handle, CURLOPT_WRITEFUNCTION, receive_incoming_data);
	thread_local_instance.receive_buffer.clear();
	if (curl_easy_perform(thread_local_instance.handle)) return std::nullopt;
	return thread_local_instance.receive_buffer;
}

/*
	SIGNATURE
*/

bool cg::sig::match(const std::string_view &signature, const char *buffer, const size_t &buffer_size) {
	size_t signature_index = 0, buffer_index = 0;
	for (;;) {
		if (signature_index + 2 > signature.size()) return false;
		if (!(signature[signature_index] == '?' && signature[signature_index + 1] == '?')) {
			char byte = double_hex_to_dec(&signature[signature_index]);
			if (buffer_index >= buffer_size || buffer[buffer_index] != byte) return false;
		}
		signature_index += 2;
		if (signature_index == signature.size() && buffer_index + 1 == buffer_size) return true;
		if (signature[signature_index] != ' ') return false;
		signature_index++;
		buffer_index++;
		if (buffer_index >= buffer_size) return false;
	}
	return false;
}

bool cg::sig::validate(const std::string_view &signature, size_t &num_bytes) {
	size_t i = 0;
	num_bytes = 0;
	for (;;) {
		if (i + 2 > signature.size()) return false;
		const char &a = signature[i];
		const char &b = signature[i + 1];
		if (!(a == '?' && b == '?')) {
			if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f') || (a >= 'A' && a <= 'F'))) return false;
			if (!((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F'))) return false;
		}
		i += 2;
		num_bytes++;
		if (i == signature.size()) return true;
		if (signature[i] != ' ') return false;
		i++;
	}
}

/*
	PROCESS
*/

bool cg::proc::query_module(const uintptr_t &vmread_query_handle, const uintptr_t &module_handle, module_info &out) {
	MODULEINFO module_info;
	if (!GetModuleInformation(reinterpret_cast<void *>(vmread_query_handle), (HMODULE)module_handle, &module_info, sizeof(MODULEINFO))) return false;
	if (out.base != reinterpret_cast<uintptr_t>(module_info.lpBaseOfDll)) return false;
	out.entry = reinterpret_cast<uintptr_t>(module_info.EntryPoint);
	out.size = module_info.SizeOfImage;
	return true;
}

std::optional<cg::proc::module_info> cg::proc::find_module(const uint32_t &process_id, const std::string_view &module_name) {
	void *snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
	if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
	MODULEENTRY32 module_entry;
	module_entry.dwSize = sizeof(MODULEENTRY32);
	if (Module32First(snapshot, &module_entry)) {
		do {
			if (strcmp(reinterpret_cast<char *>(module_entry.szModule), module_name.data()) == 0) {
				module_info result;
				result.handle = reinterpret_cast<uintptr_t>(module_entry.hModule);
				result.base = reinterpret_cast<uintptr_t>(module_entry.modBaseAddr);
				CloseHandle(snapshot);
				return result;
			}
		} while (Module32Next(snapshot, &module_entry));
	}
	CloseHandle(snapshot);
	return std::nullopt;
}

cg::proc::handle::handle(uint32_t process_id) : vmread_query(reinterpret_cast<uintptr_t>(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, process_id))) {

}

cg::proc::handle::~handle() {
	if (vmread_query) CloseHandle(reinterpret_cast<void *>(vmread_query));
}

std::optional<std::vector<uint32_t>> cg::proc::get_id(const std::string_view &exe_filename) {
	void *snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);
	std::vector<uint32_t> results;
	if (Process32First(snapshot, &entry))
		while (Process32Next(snapshot, &entry))
			if (strcmp(reinterpret_cast<char *>(entry.szExeFile), exe_filename.data()) == 0)
				results.push_back(entry.th32ProcessID);
	CloseHandle(snapshot);
	return std::move(results);
}

/*
	MEMORY
*/

bool cg::mem::read(const uintptr_t &process_handle, const uintptr_t &address, const size_t &length, void *output) {
	size_t num_read;
	if (!ReadProcessMemory(reinterpret_cast<void *>(process_handle), reinterpret_cast<void *>(address), output, length, &num_read) || num_read != length) return false;
	else return true;
}

uintptr_t cg::mem::load_effective_address(const uintptr_t &process_handle, const uintptr_t &address) {
	const uint32_t opcode_length = 3, param_length = 4, instruction_length = opcode_length + param_length;
	uint32_t operand;
	void *target = reinterpret_cast<void *>(address + opcode_length);
	if (!read(process_handle, reinterpret_cast<uintptr_t>(target), sizeof(uint32_t), &operand));
	uint64_t operand64 = operand;
	if (operand64 > std::numeric_limits<int32_t>::max()) operand64 = 0xffffffff00000000 | operand64;
	return address + operand64 + instruction_length;
}

void cg::mem::enumerate_memory_pages(const uintptr_t &process_handle, const uintptr_t &start_address, std::function<void(const uintptr_t &, const ptrdiff_t &)> callback) {
	for (uintptr_t i = start_address;;) {
		void *location = reinterpret_cast<void *>(i);
		MEMORY_BASIC_INFORMATION memory_information;
		ZeroMemory(&memory_information, sizeof(MEMORY_BASIC_INFORMATION));
		if (VirtualQueryEx(reinterpret_cast<void *>(process_handle), location, &memory_information, sizeof(MEMORY_BASIC_INFORMATION)) == 0) break;
		i = reinterpret_cast<uintptr_t>(memory_information.BaseAddress) + memory_information.RegionSize;
		if (memory_information.State != MEM_COMMIT) continue;
		callback(reinterpret_cast<uintptr_t>(memory_information.BaseAddress), memory_information.RegionSize);
	}
}

/*
	ESP OVERLAY
*/

namespace cg::esp {

	bool blocking_enabled = false;
	GLFWwindow *window_handle = 0;
	HWND native_window_handle = 0;
	LONG default_window_flags = 0;
	std::pair<int, int> target_window_frame;

	bool wrangle_gl_extensions() {
		static bool once = false;
		if (once) return true;
		glewExperimental = true;
		if (glewInit()) return false;
		std::cout << "OpenGL: " << glGetString(GL_VERSION) << " (" << glGetString(GL_RENDERER) << ")" << std::endl;
		return true;
	}

	void process_block_toggle_key() {
		static bool pressed = false;
		if (GetAsyncKeyState(VK_INSERT) != 0) {
			if (!pressed) {
				set_blocking(!blocking_enabled);
				pressed = true;
			}
		} else pressed = false;
	}
}

void *cg::esp::window() {
	return window_handle;
}

void *cg::esp::native_window() {
	return native_window_handle;
}

bool cg::esp::init_overlay(const std::string_view &title) {
	cg::esp::cleanup();
	if (!glfwInit()) return false;
	glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
	window_handle = glfwCreateWindow(200, 200, title.data(), 0, 0);
	if (!window_handle) return false;
	native_window_handle = glfwGetWin32Window(window_handle);
	if (native_window_handle) {
		default_window_flags = GetWindowLong(native_window_handle, GWL_EXSTYLE);
		set_blocking(false);
		glfwMakeContextCurrent(window_handle);
		glfwSwapInterval(0);
		if (wrangle_gl_extensions()) {
			return true;
		}
	}
	cg::esp::cleanup();
	return false;
}

bool cg::esp::poll(const std::string_view &target_window_title) {
	glfwPollEvents();
	if (glfwWindowShouldClose(window_handle)) return false;
	auto target_window_handle = FindWindowA(0, "Sea of Thieves");
	if (!target_window_handle) return false;
	RECT target_frame;
	if (DwmGetWindowAttribute(target_window_handle, DWMWA_EXTENDED_FRAME_BOUNDS, &target_frame, sizeof(RECT)) != S_OK) return false;
	target_frame.top += 33;
	target_frame.left++;
	target_frame.right--;
	target_frame.bottom--;
	glfwSetWindowPos(window_handle, target_frame.left, target_frame.top);
	glfwSetWindowSize(window_handle, target_frame.right - target_frame.left, target_frame.bottom - target_frame.top);
	target_window_frame.first = target_frame.right - target_frame.left;
	target_window_frame.second = target_frame.bottom - target_frame.top;
	process_block_toggle_key();
	return true;
}

void cg::esp::swap() {
	glfwSwapBuffers(window_handle);
}

const std::pair<int, int> &cg::esp::frame() {
	return target_window_frame;
}

void cg::esp::cleanup() {
	glfwTerminate();
	native_window_handle = 0;
	window_handle = 0;
}

void cg::esp::set_blocking(const bool &enabled) {
	if (enabled) {
		SetWindowLong(native_window_handle, GWL_EXSTYLE, default_window_flags);
		SetLayeredWindowAttributes(native_window_handle, 0, 0, 0);
	} else {
		SetWindowLong(native_window_handle, GWL_EXSTYLE, default_window_flags | WS_EX_TRANSPARENT | WS_EX_LAYERED);
		SetLayeredWindowAttributes(native_window_handle, 0, 0, 0);
	}
	blocking_enabled = enabled;
}

const bool &cg::esp::blocking() {
	return blocking_enabled;
}