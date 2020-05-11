#include <functional>
#include <vector>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

namespace cg {

	void show_dumb_banner();

	constexpr char hex_to_dec(const char &hex);
	constexpr char double_hex_to_dec(const char *hex);

	std::vector<std::string> split_tokens(const std::string_view &token, const std::string_view &source);
}

namespace cg::curl {

	std::optional<std::vector<char>> get(const std::string_view &url);
}

namespace cg::sig {

	bool match(const std::string_view &signature, const char *buffer, const size_t &buffer_size);
	bool validate(const std::string_view &signature, size_t &num_bytes);
}

namespace cg::proc {

	struct module_info {
		uintptr_t handle = 0, entry = 0, base = 0;
		ptrdiff_t size = 0;
	};

	bool query_module(const uintptr_t &vmread_query_handle, const uintptr_t &module_handle, module_info &out);

	std::optional<module_info> find_module(const uint32_t &process_id, const std::string_view &module_name);

	struct handle {

		uintptr_t const vmread_query;

		handle(uint32_t process_id);
		~handle();
	};

	std::optional<std::vector<uint32_t>> get_id(const std::string_view &exe_filename);
}

namespace cg::mem {

	bool read(const uintptr_t &process_handle, const uintptr_t &address, const size_t &length, void *output);

	template<typename T>
	bool read(const uintptr_t &process_handle, const uintptr_t &address, T &out) {
		return read(process_handle, address, sizeof(T), &out);
	}

	uintptr_t load_effective_address(const uintptr_t &process_handle, const uintptr_t &address);
	void enumerate_memory_pages(const uintptr_t &process_handle, const uintptr_t &start_address, std::function<void(const uintptr_t &, const ptrdiff_t &)> callback);
}

namespace cg::esp {

	void *window();
	void *native_window();
	bool init_overlay(const std::string_view &title);
	bool poll(const std::string_view &target_window_title);
	void swap();
	const std::pair<int, int> &frame();
	void cleanup();
	void set_blocking(const bool &enabled);
	const bool &blocking();
}