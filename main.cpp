#include <rang.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "cg.h"

#include <gl/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fmt/format.h>
#include <json.hpp>

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <map>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>

#define M_PI 3.14159265358979323846

using rang::fg;
using std::cout;
using std::wcout;
using std::endl;
using fmt::format;

namespace engine {

	const uintptr_t world_owning_game_instance_offset = 0x1c0;
	const uintptr_t world_persistent_level_offset = 0x30;
	const uintptr_t persistent_level_actors_offset = 0xa0;
	const uintptr_t owning_game_instance_local_player_offset = 0x38;
	const uintptr_t actor_id_offset = 0x18;
	const uintptr_t actor_linear_velocity_offset = 0xa4;
	const uintptr_t actor_angular_velocity_offset = 0xb0;
	const uintptr_t actor_location_offset = 0xbc;
	const uintptr_t actor_rotation_offset = 0xc8;
	const uintptr_t actor_root_scene_component_offset = 0x170;
	const uintptr_t local_player_player_controller_offset = 0x30;
	const uintptr_t player_controller_actor_offset = 0x418;
	const uintptr_t player_controller_player_state_offset = 0x430;
	const uintptr_t player_controller_camera_manager_offset = 0x4a0;
	const uintptr_t player_state_player_name_offset = 0x418;
	const uintptr_t camera_manager_x_offset = 0x490;
	const uintptr_t camera_manager_pitch_offset = 0x49c;
	const uintptr_t camera_manager_fov_offset = 0x4b8;
	const uintptr_t scene_component_bounds_origin_offset = 0x100;
	const uintptr_t scene_component_bounds_extent_offset = 0x10c;
	const uintptr_t scene_component_bounds_radius_offset = 0x118;
	const uintptr_t scene_component_relative_location_offset = 0x128;
	const uintptr_t scene_component_relative_rotation_offset = 0x134;
	const uintptr_t scene_component_velocity_offset = 0x22c;

	uintptr_t names_address;
	uintptr_t world_address;
	uintptr_t persistent_level_address;
	uintptr_t owning_game_instance_address;
	uintptr_t local_player_address;
	uintptr_t local_player_controller_address;
	uintptr_t local_player_actor_address;
	uintptr_t local_player_actor_root_component_address;
	uintptr_t local_player_camera_manager_address;
	uintptr_t local_player_state_address;
	uintptr_t local_player_name_address;

	glm::vec2 viewport(1.f, 1.f);
	wchar_t local_player_name[64] = { 0 };
	float local_player_camera_fov = 45.f;
	glm::vec3 local_player_camera_location, local_player_camera_rotation; //, local_player_camera_look;
	glm::mat3x4 local_player_camera_rotation_matrix;
	ImDrawList *foreground = 0, *background = 0;

	void update_local_player_camera_rotation_matrix() {
		auto pitch = glm::radians(local_player_camera_rotation.x);
		auto yaw = glm::radians(local_player_camera_rotation.y);
		auto roll = glm::radians(local_player_camera_rotation.z);
		auto SP = sinf(pitch), CP = cosf(pitch);
		auto SY = sinf(yaw), CY = cosf(yaw);
		auto SR = sinf(roll), CR = cosf(roll);
		local_player_camera_rotation_matrix[0][0] = CP * CY;
		local_player_camera_rotation_matrix[0][1] = CP * SY;
		local_player_camera_rotation_matrix[0][2] = SP;
		local_player_camera_rotation_matrix[0][3] = 0.f;
		local_player_camera_rotation_matrix[1][0] = SR * SP * CY - CR * SY;
		local_player_camera_rotation_matrix[1][1] = SR * SP * SY + CR * CY;
		local_player_camera_rotation_matrix[1][2] = -SR * CP;
		local_player_camera_rotation_matrix[1][3] = 0.f;
		local_player_camera_rotation_matrix[2][0] = -(CR * SP * CY + SR * SY);
		local_player_camera_rotation_matrix[2][1] = CY * SR - CR * SP * SY;
		local_player_camera_rotation_matrix[2][2] = CR * CP;
		local_player_camera_rotation_matrix[2][3] = 0.f;
	}

	struct signature {
		std::string pattern;
		size_t num_bytes;
		uintptr_t remote_location;
	};

	void resolve_signature_locations(uintptr_t process_handle, cg::proc::module_info &module, std::map<std::string, signature> &signatures) {
		std::vector<char> region_data_buffer;
		cg::mem::enumerate_memory_pages(process_handle, module.base, [&](const uintptr_t &region_base, const ptrdiff_t &region_size) {
			auto signatures_to_find = std::find_if_not(signatures.begin(), signatures.end(), [](const std::pair<std::string, signature> &item) -> bool { return item.second.remote_location; });
			if (signatures_to_find == signatures.end()) return;
			size_t num_read;
			if (region_data_buffer.size() < region_size) region_data_buffer.resize(region_size);
			if (ReadProcessMemory(reinterpret_cast<void *>(process_handle), reinterpret_cast<void *>(region_base), region_data_buffer.data(), region_size, &num_read)) {
				for (std::map<std::string, signature>::iterator i = signatures.begin(); i != signatures.end(); i++) {
					auto &signature = signatures[i->first];
					if (signature.remote_location) continue;
					for (size_t region_byte_index = 0; region_byte_index < num_read - signature.num_bytes; region_byte_index++) {
						if (cg::sig::match(signature.pattern, &region_data_buffer[region_byte_index], signature.num_bytes)) {
							signature.remote_location = cg::mem::load_effective_address(process_handle, reinterpret_cast<uintptr_t>(region_base) + region_byte_index);
							cout << "Found " << fg::green <<  i->first << fg::reset << " signature at " << fg::yellow << reinterpret_cast<void *>(signature.remote_location) << fg::reset << endl;
							break;
						}
					}
				}
			}
		});
	}

	std::map<std::string, signature> &signatures() {
		static bool first = true;
		static std::map<std::string, signature> signatures;
		if (first) {
			signatures["GNames"] = []() {
				signature new_sig;
				new_sig.pattern = "48 8B 1D ?? ?? ?? ?? 48 85 ?? 75 3A";
				new_sig.remote_location = 0;
				cout << "GNames pattern: " << (cg::sig::validate(new_sig.pattern, new_sig.num_bytes) ? "Valid" : "Invalid") << endl;
				return new_sig;
			}();
			signatures["UWorld"] = []() {
				signature new_sig;
				new_sig.pattern = "48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 74 06 48 8B 49 70";
				new_sig.remote_location = 0;
				cout << "UWorld pattern: " << (cg::sig::validate(new_sig.pattern, new_sig.num_bytes) ? "Valid" : "Invalid") << endl;
				return new_sig;
			}();
			first = false;
		}
		return signatures;
	}

	/* Converts 3D world coordinates to 2D screen coordinates.
	The local_player_camera_rotation_matrix is updated once per frame.*/
	glm::vec2 project(const glm::vec3 &location) {
		glm::vec3 axis[] = {
			{ local_player_camera_rotation_matrix[0][0], local_player_camera_rotation_matrix[0][1], local_player_camera_rotation_matrix[0][2] },
			{ local_player_camera_rotation_matrix[1][0], local_player_camera_rotation_matrix[1][1], local_player_camera_rotation_matrix[1][2] },
			{ local_player_camera_rotation_matrix[2][0], local_player_camera_rotation_matrix[2][1], local_player_camera_rotation_matrix[2][2] }
		};
		glm::vec3 location_delta = location - local_player_camera_location;
		glm::vec3 screen_point = glm::vec3(glm::dot(location_delta, axis[1]), glm::dot(location_delta, axis[2]), glm::dot(location_delta, axis[0]));
		if (screen_point.z < 1.f) screen_point.z = 1.f;
		auto aspect_based_fov = (viewport.x / viewport.y) / (16.0f / 9.0f) * tanf(local_player_camera_fov * M_PI / 360.0f);
		return {
			(viewport.x * .5f) + screen_point.x * (viewport.x * .5f) / aspect_based_fov / screen_point.z,
			(viewport.y * .5f) - screen_point.y * (viewport.x * .5f) / aspect_based_fov / screen_point.z
		};
	}

	struct array {

		// These represent the memory structure of TArray.

		uintptr_t data_address;
		int32_t size;
		int32_t allocated_size;

		static void walk(uintptr_t process_handle, uintptr_t names_address, uintptr_t address, std::function<void(const uintptr_t &address)> per_index_callback) {
			array working;
			if (!cg::mem::read(process_handle, address, sizeof(array), &working)) return;
			std::vector<uintptr_t> list;
			list.resize(working.size);
			if (!cg::mem::read(process_handle, working.data_address, sizeof(uintptr_t) * list.size(), list.data())) return;
			for (auto &pointer_value : list) per_index_callback(pointer_value);
		}
	};

	struct actor {

		// These are for convenience and don't represent the actual structure.

		uintptr_t component_address;
		glm::fvec3 component_bounds_origin, component_bounds_extent;
		float component_bounds_radius;
		glm::vec3 linear_velocity, angular_velocity;
		glm::vec3 location, rotation;
		glm::vec3 component_relative_location, component_relative_rotation;
		glm::vec3 component_velocity;

		static std::optional<actor> from(const uintptr_t &process_handle, const uintptr_t &actor_address) {
			actor result;
			if (!cg::mem::read(process_handle, actor_address + actor_root_scene_component_offset, result.component_address)) return std::nullopt;
			if (!cg::mem::read(process_handle, actor_address + actor_location_offset, result.location)) return std::nullopt;
			if (!cg::mem::read(process_handle, actor_address + actor_rotation_offset, result.rotation)) return std::nullopt;
			if (!cg::mem::read(process_handle, actor_address + actor_linear_velocity_offset, result.linear_velocity)) return std::nullopt;
			if (!cg::mem::read(process_handle, actor_address + actor_angular_velocity_offset, result.angular_velocity)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_bounds_origin_offset, result.component_bounds_origin)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_bounds_extent_offset, result.component_bounds_extent)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_bounds_radius_offset, result.component_bounds_radius)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_relative_location_offset, result.component_relative_location)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_relative_rotation_offset, result.component_relative_rotation)) return std::nullopt;
			if (!cg::mem::read(process_handle, result.component_address + scene_component_velocity_offset, result.component_velocity)) return std::nullopt;
			return std::move(result);
		}
	};

	/* Essentially storing a copy of GNames within our process so we don't have to
	resort to RPM to lookup an actor name every single time. Dramitically reducing
	calls to RPM per frame.*/

	std::unordered_map<int32_t, std::string> actor_id_cache;

	// Refers to actor_id_cache first. If it's not in there it searches GNames in the target process.
	std::optional<std::string> get_name(const uintptr_t &process_handle, const uintptr_t &names_address, const int32_t &id) {
		uintptr_t name_ptr, name_address;
		if (actor_id_cache.find(id) != actor_id_cache.end()) return actor_id_cache[id];
		if (!cg::mem::read(process_handle, names_address + int32_t(id / 0x4000) * 0x8, sizeof(uintptr_t), &name_ptr)) return std::nullopt;
		if (!cg::mem::read(process_handle, name_ptr + 0x8 * int32_t(id % 0x4000), sizeof(uintptr_t), &name_address)) return std::nullopt;
		char name_buffer[128] = { 0 };
		if (!cg::mem::read(process_handle, name_address + 0x10, sizeof(name_buffer), name_buffer)) return std::nullopt;
		if (actor_id_cache.find(id) == actor_id_cache.end()) actor_id_cache[id] = name_buffer;
		return std::move(std::string(name_buffer));
	}

	void enumerate_actors(const uintptr_t process_handle, std::function<void(const uintptr_t &address, const int32_t &id, const std::string_view &name)> per_actor_callback) {
		array::walk(process_handle, names_address, persistent_level_address + persistent_level_actors_offset, [&](const uintptr_t &address) {
			int32_t id;
			if (!cg::mem::read(process_handle, address + actor_id_offset, id)) return;
			auto my_name = get_name(process_handle, names_address, id);
			if (!my_name) return;
			per_actor_callback(address, id, *my_name);
		});
	}
}

namespace tools {

	ImFont *general_font = 0, *ui_font = 0;

	nlohmann::json settings;

	bool show_actor_explorer = false;
	bool force_debug_draw_on_all_actors = false;

	bool init_gui() {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		ImGui_ImplGlfw_InitForOpenGL((GLFWwindow *)cg::esp::window(), true);
		ImGui_ImplOpenGL3_Init("#version 130");
		general_font = ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 14, 0, 0);
		ui_font = ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 14, 0, 0);
		return true;
	}

	void load_settings() {
		if (!std::filesystem::exists("settings.json")) {
			std::ofstream out("settings.json");
			out << "null";
			cout << "Made new settings file." << endl;
		} else {
			std::ifstream file("settings.json");
			std::stringstream buffer;
			buffer << file.rdbuf();
			std::string content = buffer.str();
			settings = nlohmann::json::parse(content.begin(), content.end());
			cout << "Loaded settings." << endl;
		}
	}

	void dump_settings() {
		std::ofstream out("settings.json");
		out << settings.dump(1, '\t', true);
		cout << "Saved settings." << endl;
	}

	void apply_extasy_hosting_style() {
		ImGui::StyleColorsDark();
		auto style = &ImGui::GetStyle();
		style->WindowPadding = ImVec2(15, 15);
		style->WindowRounding = 0.0f;
		style->FramePadding = ImVec2(5, 5);
		style->FrameRounding = 0.0f;
		style->ItemSpacing = ImVec2(12, 8);
		style->ItemInnerSpacing = ImVec2(8, 6);
		style->IndentSpacing = 25.0f;
		style->ScrollbarSize = 15.0f;
		style->ScrollbarRounding = 0.0f;
		style->GrabMinSize = 5.0f;
		style->GrabRounding = 0.0f;
		style->TabRounding = 0.0f;
		auto *colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		colors[ImGuiCol_Border] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 1.00f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(1.00f, 0.98f, 0.95f, 1.00f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_Button] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_Header] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 1.00f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.35f, 0.58f, 1.00f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.07f, 0.10f, 0.15f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
		colors[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.39f, 0.38f, 1.00f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.40f, 0.39f, 0.38f, 1.00f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(1.00f, 0.98f, 0.95f, 1.00f);
		for (int i = 0; i < ImGuiCol_COUNT; i++) colors[i].w = 1.f; // Disable all transparency.
	}

	void apply_metasprite_classic_style() {
		ImGui::StyleColorsDark();
		auto style = &ImGui::GetStyle();
		style->WindowPadding = ImVec2(3, 3);
		style->WindowRounding = 0.0f;
		style->FramePadding = ImVec2(3, 3);
		style->FrameRounding = 0.0f;
		style->ItemSpacing = ImVec2(5, 5);
		style->ItemInnerSpacing = ImVec2(3, 3);
		style->IndentSpacing = 3.0f;
		style->ScrollbarSize = 11.0f;
		style->ScrollbarRounding = 0.0f;
		style->GrabMinSize = 7.0f;
		style->GrabRounding = 0.0f;
		style->TabRounding = 0.0f;
		auto *colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.29f, 0.34f, 0.26f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.29f, 0.34f, 0.26f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.24f, 0.27f, 0.20f, 1.00f);
		colors[ImGuiCol_Border] = ImVec4(0.54f, 0.57f, 0.51f, 0.50f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.14f, 0.16f, 0.11f, 0.52f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.24f, 0.27f, 0.20f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.30f, 0.23f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.34f, 0.26f, 1.00f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.24f, 0.27f, 0.20f, 1.00f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.29f, 0.34f, 0.26f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.24f, 0.27f, 0.20f, 1.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.32f, 0.24f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.30f, 0.22f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.23f, 0.27f, 0.21f, 1.00f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.54f, 0.57f, 0.51f, 0.50f);
		colors[ImGuiCol_Button] = ImVec4(0.29f, 0.34f, 0.26f, 0.40f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.54f, 0.57f, 0.51f, 0.50f);
		colors[ImGuiCol_Header] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.42f, 0.31f, 0.6f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.54f, 0.57f, 0.51f, 0.50f);
		colors[ImGuiCol_Separator] = ImVec4(0.14f, 0.16f, 0.11f, 1.00f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.54f, 0.57f, 0.51f, 1.00f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.19f, 0.23f, 0.18f, 0.00f); // grip invis
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.54f, 0.57f, 0.51f, 1.00f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_Tab] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.54f, 0.57f, 0.51f, 0.78f);
		colors[ImGuiCol_TabActive] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.24f, 0.27f, 0.20f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.35f, 0.42f, 0.31f, 1.00f);
		colors[ImGuiCol_DockingPreview] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.78f, 0.28f, 1.00f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(0.73f, 0.67f, 0.24f, 1.00f);
		colors[ImGuiCol_NavHighlight] = ImVec4(0.59f, 0.54f, 0.18f, 1.00f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
		for (int i = 0; i < ImGuiCol_COUNT; i++) colors[i].w = 1.f; // Disable all transparency.
	}

	void render_menu() {
		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::MenuItem(format("{} Actor Explorer", show_actor_explorer ? "Hide" : "Show").c_str(), 0, &show_actor_explorer));
			if (ImGui::BeginMenu("Theme")) {
				if (ImGui::MenuItem("Apply Classic Theme")) apply_metasprite_classic_style();
				if (ImGui::MenuItem("Apply Extasy Theme")) apply_extasy_hosting_style();
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Save")) dump_settings();
			ImGui::EndMainMenuBar();
		}
	}

	// Extra messy. Needs clean up.
	void render_actor_explorer() {
		// ImGui::ShowDemoWindow();
		static char name_filter[128] = { 0 };
		static char new_group_name[128] = { 0 };
		ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Actor Explorer")) {
			ImGui::BeginTabBar("Tabs");
			if (ImGui::BeginTabItem("Global Actor List")) {
				ImGui::Text(format("{} actors in cache.", engine::actor_id_cache.size()).c_str());
				ImGui::Checkbox("Expose All Actors", &force_debug_draw_on_all_actors);
				ImGui::InputTextWithHint("Filter", "Name", name_filter, sizeof(name_filter), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::BeginChild("actor_list", { }, true);
				for (auto actor : engine::actor_id_cache) {
					if (name_filter) {
						if (actor.second.find(name_filter) == std::string::npos) continue;
					}
					std::vector<std::string> in_groups;
					std::string in_groups_summary;
					for (auto i = settings["render_groups"].begin(); i != settings["render_groups"].end(); i++)
						for (std::string member_name : i.value()["members"])
							if (member_name == actor.second) {
								in_groups.push_back(i.key());
								in_groups_summary += "[" + i.key() + "] ";
							}
					ImGui::PushItemWidth(200);
					if (ImGui::BeginCombo(format("{} ({})", actor.second, actor.first).c_str(), in_groups_summary.c_str())) {
						int uid = 0;
						for (auto i = settings["render_groups"].begin(); i != settings["render_groups"].end(); i++) {
							if (std::find(in_groups.begin(), in_groups.end(), i.key()) == in_groups.end()) {
								if (ImGui::Selectable(format("Add to \"{}\"##{}", i.key(), uid).c_str())) {
									i.value()["members"].push_back(actor.second);
								}
								uid++;
							} else {
								if (ImGui::Selectable(format("Remove from \"{}\"##{}", i.key(), uid).c_str())) {
									std::vector<std::string> members = i.value()["members"];
									members.erase(std::find(members.begin(), members.end(), actor.second));
									i.value()["members"] = members;
								}
								uid++;
							}
						}
						ImGui::EndCombo();
					}
					ImGui::PopItemWidth();
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Render Groups")) {
				if (ImGui::InputTextWithHint("Create New Group", "Group Name", new_group_name, sizeof(new_group_name), ImGuiInputTextFlags_EnterReturnsTrue)) {
					if (new_group_name[0] && std::find(settings["render_groups"].begin(), settings["render_groups"].end(), new_group_name) == settings["render_groups"].end()) {
						settings["render_groups"][new_group_name] = { };
						memset(new_group_name, 0, sizeof(new_group_name));
					}
				}
				static std::string selected_group;
				ImGui::BeginChild("List", { 200, 0 }, true);
				if (settings["render_groups"].size()) {
					for (nlohmann::json::iterator i = settings["render_groups"].begin(); i != settings["render_groups"].end(); i++) {
						if (ImGui::Selectable(i.key().c_str(), selected_group != "" && i.key() == selected_group)) selected_group = i.key();
					}
				}
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("Group Info", { }, true);
				if (settings["render_groups"].contains(selected_group)) {
					float text_color[4] = { 1.f, 0.f, 0.f, 1.f };
					float misc_color[4] = { 1.f, 0.f, 0.f, 1.f };
					if (settings["render_groups"][selected_group].contains("text_color")) {
						text_color[0] = static_cast<float>(settings["render_groups"][selected_group]["text_color"]["r"]);
						text_color[1] = static_cast<float>(settings["render_groups"][selected_group]["text_color"]["g"]);
						text_color[2] = static_cast<float>(settings["render_groups"][selected_group]["text_color"]["b"]);
						text_color[3] = static_cast<float>(settings["render_groups"][selected_group]["text_color"]["a"]);
					}
					if (settings["render_groups"][selected_group].contains("misc_color")) {
						misc_color[0] = static_cast<float>(settings["render_groups"][selected_group]["misc_color"]["r"]);
						misc_color[1] = static_cast<float>(settings["render_groups"][selected_group]["misc_color"]["g"]);
						misc_color[2] = static_cast<float>(settings["render_groups"][selected_group]["misc_color"]["b"]);
						misc_color[3] = static_cast<float>(settings["render_groups"][selected_group]["misc_color"]["a"]);
					}
					ImGui::PushItemWidth(100);
					if (ImGui::ColorPicker4("Text Color", text_color)) {
						settings["render_groups"][selected_group]["text_color"] = {
							{ "r", text_color[0] },
							{ "g", text_color[1] },
							{ "b", text_color[2] },
							{ "a", text_color[3] }
						};
					}
					ImGui::SameLine();
					if (ImGui::ColorPicker4("Misc Color", misc_color)) {
						settings["render_groups"][selected_group]["misc_color"] = {
							{ "r", misc_color[0] },
							{ "g", misc_color[1] },
							{ "b", misc_color[2] },
							{ "a", misc_color[3] }
						};
					}
					ImGui::PopItemWidth();
					bool draw_name = settings["render_groups"][selected_group].contains("draw_name") ? static_cast<bool>(settings["render_groups"][selected_group]["draw_name"]) : false;
					bool draw_origin = settings["render_groups"][selected_group].contains("draw_origin") ? static_cast<bool>(settings["render_groups"][selected_group]["draw_origin"]) : false;
					bool draw_extents = settings["render_groups"][selected_group].contains("draw_extents") ? static_cast<bool>(settings["render_groups"][selected_group]["draw_extents"]) : false;
					bool use_flat_extents = settings["render_groups"][selected_group].contains("use_flat_extents") ? static_cast<bool>(settings["render_groups"][selected_group]["use_flat_extents"]) : false;
					int border_thickness = settings["render_groups"][selected_group].contains("border_thickness") ? static_cast<int>(settings["render_groups"][selected_group]["border_thickness"]) : 1;
					if (ImGui::Checkbox("Draw Name", &draw_name)) settings["render_groups"][selected_group]["draw_name"] = draw_name;
					if (ImGui::Checkbox("Draw Origin", &draw_origin)) settings["render_groups"][selected_group]["draw_origin"] = draw_origin;
					if (ImGui::Checkbox("Draw Extents", &draw_extents)) settings["render_groups"][selected_group]["draw_extents"] = draw_extents;
					if (ImGui::Checkbox("Use Flat Extents", &use_flat_extents)) settings["render_groups"][selected_group]["use_flat_extents"] = use_flat_extents;
					ImGui::PushItemWidth(120);
					if (ImGui::InputInt("Border Thickness", &border_thickness)) settings["render_groups"][selected_group]["border_thickness"] = border_thickness;
					ImGui::PopItemWidth();
					ImGui::Separator();
					ImGui::BeginChild("Member List", { 0, 300 }, true);
					ImGui::PushItemWidth(200);
					/* Add & remove members from this group.
					We're making a copy here because iterating over a vector while you're
					adding and removing things from it is a bad idea. */
					std::vector<std::string> copy_of_members = settings["render_groups"][selected_group]["members"];
					for (const std::string_view &member : copy_of_members) {
						std::vector<std::string> in_groups;
						std::string in_groups_summary;
						for (auto i = settings["render_groups"].begin(); i != settings["render_groups"].end(); i++)
							for (std::string member_name : i.value()["members"])
								if (member_name == member) {
									in_groups.push_back(i.key());
									in_groups_summary += "[" + i.key() + "] ";
								}
						if (ImGui::BeginCombo(member.data(), in_groups_summary.c_str())) {
							for (auto i = settings["render_groups"].begin(); i != settings["render_groups"].end(); i++) {
								if (std::find(in_groups.begin(), in_groups.end(), i.key()) == in_groups.end()) {
									if (ImGui::Selectable(format("Add to \"{}\"", i.key()).c_str())) i.value()["members"].push_back(member);
								} else {
									if (ImGui::Selectable(format("Remove from \"{}\"", i.key()).c_str())) {
										std::vector<std::string> working_members = i.value()["members"];
										working_members.erase(std::find(working_members.begin(), working_members.end(), member));
										i.value()["members"] = working_members;
									}
								}
							}
							ImGui::EndCombo();
						}
					}
					ImGui::PopItemWidth();
					ImGui::EndChild();
					ImGui::Separator();
					if (ImGui::Button("Delete Group")) {
						settings["render_groups"].erase(selected_group);
						selected_group = "";
					}
				} else ImGui::Text("Select a group or make a new one.");
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	void render() {
		render_menu();
		if (show_actor_explorer) render_actor_explorer();
	}
}

int update_addresses(const uintptr_t &process_handle, const engine::signature &world, const engine::signature &names) {
	if (!cg::mem::read(process_handle, names.remote_location, engine::names_address)) return 1;
	if (!cg::mem::read(process_handle, world.remote_location, engine::world_address)) return 2;
	if (!cg::mem::read(process_handle, engine::world_address + engine::world_persistent_level_offset, engine::persistent_level_address)) return 3;
	if (!cg::mem::read(process_handle, engine::world_address + engine::world_owning_game_instance_offset, engine::owning_game_instance_address)) return 4;
	if (!cg::mem::read(process_handle, engine::owning_game_instance_address + engine::owning_game_instance_local_player_offset, engine::local_player_address)) return 5;
	if (!cg::mem::read(process_handle, engine::local_player_address, engine::local_player_address)) return 6;
	if (!cg::mem::read(process_handle, engine::local_player_address + engine::local_player_player_controller_offset, engine::local_player_controller_address)) return 7;
	if (!cg::mem::read(process_handle, engine::local_player_controller_address + engine::player_controller_actor_offset, engine::local_player_actor_address)) return 8;
	if (!cg::mem::read(process_handle, engine::local_player_actor_address + engine::actor_root_scene_component_offset, engine::local_player_actor_root_component_address)) return 9;
	if (!cg::mem::read(process_handle, engine::local_player_controller_address + engine::player_controller_camera_manager_offset, engine::local_player_camera_manager_address)) return 10;
	if (!cg::mem::read(process_handle, engine::local_player_controller_address + engine::player_controller_player_state_offset, engine::local_player_state_address)) return 11;
	if (!cg::mem::read(process_handle, engine::local_player_state_address + engine::player_state_player_name_offset, engine::local_player_name_address)) return 12;
	if (!cg::mem::read(process_handle, engine::local_player_name_address, sizeof(engine::local_player_name), engine::local_player_name)) return 13;
	if (!cg::mem::read(process_handle, engine::local_player_camera_manager_address + engine::camera_manager_x_offset, engine::local_player_camera_location)) return 14;
	if (!cg::mem::read(process_handle, engine::local_player_camera_manager_address + engine::camera_manager_pitch_offset, engine::local_player_camera_rotation)) return 15;
	if (!cg::mem::read(process_handle, engine::local_player_camera_manager_address + engine::camera_manager_fov_offset, engine::local_player_camera_fov)) return 16;
	engine::update_local_player_camera_rotation_matrix();
	return 0;
}

void begin_frame() {
	engine::viewport.x = cg::esp::frame().first;
	engine::viewport.y = cg::esp::frame().second;
	glViewport(0, 0, engine::viewport.x, engine::viewport.y);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
	engine::foreground = ImGui::GetForegroundDrawList();
	engine::background = ImGui::GetBackgroundDrawList();
}

void end_frame() {
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

int main(int, char **) {

	tools::load_settings();
	cg::show_dumb_banner();

	auto pids = cg::proc::get_id("SoTGame.exe");
	if (!(pids && pids->size())) return 1;
	auto module = cg::proc::find_module(pids->front(), "SoTGame.exe");
	if (!module) return 2;
	cg::proc::handle handle(pids->front());
	if (!handle.vmread_query) return 3;
	if (!cg::proc::query_module(handle.vmread_query, module->handle, *module)) return 4;

	cout << "Process ID: " << reinterpret_cast<void *>(pids->front()) << endl;
	cout << "Module Handle: " << reinterpret_cast<void *>(module->handle) << endl;
	cout << "Module Base: " << reinterpret_cast<void *>(module->base) << endl;
	cout << "Module Entry Point: " << reinterpret_cast<void *>(module->entry) << endl;
	cout << "Module Size: " << reinterpret_cast<void *>(module->size) << endl;

	resolve_signature_locations(handle.vmread_query, *module, engine::signatures());

	if (!cg::esp::init_overlay("SOTESP")) return 180;

	tools::init_gui();
	tools::apply_metasprite_classic_style();

	LARGE_INTEGER frequency, frame_start_time;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&frame_start_time);

	while (cg::esp::poll("Sea of Thieves")) {

		/* Keep trying to get good addresses until there are no errors.
		We're basically dealing with race conditions constantly since this hack is external.*/

		while (update_addresses(handle.vmread_query, engine::signatures()["UWorld"], engine::signatures()["GNames"]));

		begin_frame();

		/* glm::vec4 look(10.f, 0.f, 0.f, 0.f);
		look = glm::rotate(glm::mat4(1.f), glm::radians(engine::local_player_camera_rotation.x), glm::fvec3(0.f, -1.f, 0.f)) * look;
		look = glm::rotate(glm::mat4(1.f), glm::radians(engine::local_player_camera_rotation.y), glm::vec3(0.f, 0.f, 1.f)) * look;
		engine::local_player_camera_look = look; */

		/* Building the actor_render_group_cache beforehand makes finding render groups that are associated with
		some actor way faster than searching all of them per actor. We can just do a quick lookup.
		Before this optimization, enumerate_actors was taking upwards of 12 ms.*/

		static std::unordered_map<std::string, std::vector<std::string>> actor_render_group_cache;

		actor_render_group_cache.clear();

		for (auto i = tools::settings["render_groups"].begin(); i != tools::settings["render_groups"].end(); i++)
			for (std::string member_name : i.value()["members"])
				actor_render_group_cache[member_name].push_back(i.key());

		engine::enumerate_actors(handle.vmread_query, [&](const uintptr_t &address, const int32_t &id, const std::string_view &name) {
			if (address == engine::local_player_actor_address) return;
			if (actor_render_group_cache.find(name.data()) == actor_render_group_cache.end()) return;
			for (auto &render_group : actor_render_group_cache[name.data()]) {
				auto settings = tools::settings["render_groups"][render_group];
				bool draw_name = settings.contains("draw_name") ? static_cast<bool>(settings["draw_name"]) : false;
				bool draw_origin = settings.contains("draw_origin") ? static_cast<bool>(settings["draw_origin"]) : false;
				bool draw_extents = settings.contains("draw_extents") ? static_cast<bool>(settings["draw_extents"]) : false;
				bool use_flat_extents = settings.contains("use_flat_extents") ? static_cast<bool>(settings["use_flat_extents"]) : false;
				int border_thickness = settings.contains("border_thickness") ? static_cast<int>(settings["border_thickness"]) : 1;
				int text_color[4] = { 255, 0, 0, 255 };
				int misc_color[4] = { 255, 0, 0, 255 };
				if (settings.contains("text_color")) {
					text_color[0] = static_cast<float>(settings["text_color"]["r"]) * 255;
					text_color[1] = static_cast<float>(settings["text_color"]["g"]) * 255;
					text_color[2] = static_cast<float>(settings["text_color"]["b"]) * 255;
					text_color[3] = static_cast<float>(settings["text_color"]["a"]) * 255;
				}
				if (settings.contains("misc_color")) {
					misc_color[0] = static_cast<float>(settings["misc_color"]["r"]) * 255;
					misc_color[1] = static_cast<float>(settings["misc_color"]["g"]) * 255;
					misc_color[2] = static_cast<float>(settings["misc_color"]["b"]) * 255;
					misc_color[3] = static_cast<float>(settings["misc_color"]["a"]) * 255;
				}
				auto actor = engine::actor::from(handle.vmread_query, address);
				if (!actor) return;
				auto place = engine::project(actor->component_bounds_origin);
				float human_readable_distance = glm::distance(actor->component_bounds_origin, engine::local_player_camera_location) * 0.01f;
				if (draw_origin && human_readable_distance < 500) {
					glm::vec2 p[] = {
						engine::project({ actor->component_bounds_origin.x, actor->component_bounds_origin.y, actor->component_bounds_origin.z }),
						engine::project({ actor->component_bounds_origin.x + actor->component_bounds_extent.x, actor->component_bounds_origin.y, actor->component_bounds_origin.z }),
						engine::project({ actor->component_bounds_origin.x, actor->component_bounds_origin.y, actor->component_bounds_origin.z }),
						engine::project({ actor->component_bounds_origin.x, actor->component_bounds_origin.y + actor->component_bounds_extent.y, actor->component_bounds_origin.z }),
						engine::project({ actor->component_bounds_origin.x, actor->component_bounds_origin.y, actor->component_bounds_origin.z }),
						engine::project({ actor->component_bounds_origin.x, actor->component_bounds_origin.y, actor->component_bounds_origin.z + actor->component_bounds_extent.z })
					};
					engine::background->AddLine({ p[0].x, p[0].y }, { p[1].x, p[1].y }, IM_COL32(255, 0, 0, 255), 2);
					engine::background->AddLine({ p[2].x, p[2].y }, { p[3].x, p[3].y }, IM_COL32(0, 255, 0, 255), 2);
					engine::background->AddLine({ p[4].x, p[4].y }, { p[5].x, p[5].y }, IM_COL32(0, 0, 255, 255), 2);
				}
				if (glm::distance(engine::local_player_camera_location, actor->component_bounds_origin) > actor->component_bounds_radius) {
					if (draw_extents) {
						glm::vec2 p[] = {
							engine::project(actor->component_bounds_origin + glm::vec3(actor->component_bounds_extent.x, -actor->component_bounds_extent.y, actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(actor->component_bounds_extent.x, actor->component_bounds_extent.y, actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(-actor->component_bounds_extent.x, -actor->component_bounds_extent.y, actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(-actor->component_bounds_extent.x, actor->component_bounds_extent.y, actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(actor->component_bounds_extent.x, -actor->component_bounds_extent.y, -actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(actor->component_bounds_extent.x, actor->component_bounds_extent.y, -actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(-actor->component_bounds_extent.x, -actor->component_bounds_extent.y, -actor->component_bounds_extent.z)),
							engine::project(actor->component_bounds_origin + glm::vec3(-actor->component_bounds_extent.x, actor->component_bounds_extent.y, -actor->component_bounds_extent.z)),
						};
						glm::vec2 min = p[0], max = p[0];
						for (auto &point : p) {
							if (point.x < min.x) min.x = point.x;
							if (point.y < min.y) min.y = point.y;
							if (point.x > max.x) max.x = point.x;
							if (point.y > max.y) max.y = point.y;
						}
						if (use_flat_extents) { // Summarize the extents into a basic rect shape instead of a 3D one.
							engine::background->AddRect({ min.x, min.y }, { max.x, max.y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), 5, ImDrawCornerFlags_All, border_thickness);
						} else {
							engine::background->AddLine({ p[0].x, p[0].y }, { p[1].x, p[1].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[1].x, p[1].y }, { p[3].x, p[3].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[2].x, p[2].y }, { p[3].x, p[3].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[2].x, p[2].y }, { p[0].x, p[0].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[4].x, p[4].y }, { p[5].x, p[5].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[5].x, p[5].y }, { p[7].x, p[7].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[6].x, p[6].y }, { p[7].x, p[7].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[6].x, p[6].y }, { p[4].x, p[4].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[0].x, p[0].y }, { p[4].x, p[4].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[1].x, p[1].y }, { p[5].x, p[5].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[2].x, p[2].y }, { p[6].x, p[6].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
							engine::background->AddLine({ p[3].x, p[3].y }, { p[7].x, p[7].y }, IM_COL32(misc_color[0], misc_color[1], misc_color[2], misc_color[3]), border_thickness);
						}
						if (draw_name) engine::background->AddText({ max.x + border_thickness + 4, min.y + ((max.y - min.y) * .5f) - 7.f }, IM_COL32(text_color[0], text_color[1], text_color[2], text_color[3]), format("{} [{:.{}f}]", name, human_readable_distance, 1).c_str(), 0);
					}
				} else if (draw_name) {
					auto top_2d = engine::project(actor->component_bounds_origin + glm::vec3(0, 0, actor->component_bounds_extent.z * .5f));
					engine::background->AddText({ top_2d.x, top_2d.y }, IM_COL32(text_color[0], text_color[1], text_color[2], text_color[3]), format("{} [{:.{}f}]", name, human_readable_distance, 1).c_str(), 0);
				}
			}
		});

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const double elapsed_frame_time = (static_cast<double>(now.QuadPart - frame_start_time.QuadPart) / static_cast<double>(frequency.QuadPart)) * 1000.0;
		frame_start_time = now;

		ImGui::PushFont(tools::ui_font);

		if (cg::esp::blocking()) tools::render();

		auto center = engine::viewport * .5f;
		engine::foreground->AddText({ 20, 20 }, IM_COL32(255, 0, 0, 255), format("Frame: {} ms", static_cast<int>(elapsed_frame_time)).c_str(), 0);
		engine::foreground->AddRect({ center.x - 4, center.y - 4 }, { center.x + 4, center.y + 4 }, IM_COL32(255, 0, 0, 192), 0.f, 0, 2.f);

		ImGui::PopFont();

		end_frame();
		cg::esp::swap();
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext(0);
	cg::esp::cleanup();
	tools::dump_settings();

	return 0;
}