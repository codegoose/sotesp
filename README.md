[![Codacy Badge](https://app.codacy.com/project/badge/Grade/3dbac95ac0164b17a32b4a36a268d80a)](https://www.codacy.com/manual/b.white.1/sotesp?utm_source=gitlab.com&amp;utm_medium=referral&amp;utm_content=codegoose/sotesp&amp;utm_campaign=Badge_Grade)

# SOTESP
### Compatible with [2.0.16](https://www.seaofthieves.com/release-notes/2.0.16) "Haunted Shores" (16th June, 2020)
This is an external hack, or cheat for Sea of Thieves that enables the player to see treasures, enemies, ships, other players and whatever else through walls and from great distances. A user interface is included that is used to manage render properties of in-game actors as well as to provide an easy-to-observe list of all in-game actors.

### Screenshots
<img src="https://gitlab.com/codegoose/sotesp/-/raw/master/screenshots/combined-preview-2.jpg" />
<img src="https://gitlab.com/codegoose/sotesp/-/raw/master/screenshots/combined-preview-1.jpg" />
Some UI screencaps can be found [here](https://imgur.com/a/9C1Ayn3).

### How's this work?
Basically, this software runs alongside the game and utilizes ```ReadProcessMemory``` from the Windows API to grab and read memory from the target process. It then walks the underlying Unreal Engine 4 architecture within the game to locate game objects and then uses a world to screen routine to highlight objects of interest within the players view.

***This is against the games TOS and you could be banned for using this*** but as far as I've experienced this is undetected. At the time of writing this, Sea of Thieves doesn't have any official anti-cheat software. Additionally, just due to the fact that is functions via a transparent overlay window, the hack is not be visible when streaming the game. ***This hack is completely external and does not write to, or inject code into the target process.*** It strictly only reads memory.

Primarily based off of Unreal Engine 4.10.1 source code; commited memory pages in the "SoTGame.exe" module of the target process are scanned for static pointers to two UE4 TArray objects: ***GNames*** is a structure which is used to map ID codes of in-game objects to human-readable names like "BP_SmallShipTemplate_C" or the like. ***UWorld*** manages information related to the current level, lists of local players, etc. But most importantly there exists a pointer to a ***ULevel*** object know as "PersistentLevel" that references a list of all ***AActor***'s in the game world. Within each ***AActor*** is an ID that can be used in conjunction with ***GNames*** to identify what exactly each "actor" is.
From here we use the structure of ***AActor*** to divulge even more information:
```c++
struct actor {
	// These don't represent the actual structure.
	uintptr_t component_address;
	glm::fvec3 component_bounds_origin, component_bounds_extent;
	float component_bounds_radius;
	glm::vec3 linear_velocity, angular_velocity;
	glm::vec3 location, rotation;
	glm::vec3 component_relative_location, component_relative_rotation;
	glm::vec3 component_velocity;
	static std::optional<actor> from(const uintptr_t &process_handle, const uintptr_t &actor_address);
}
```

All ***AActor***'s have a reference to a "RootSceneComponent" that contains additional information related to actually rendering the ***AActor***. That's what the "component_*" variables are referencing and is of the type ***USceneComponent*** in the source. Process this information and then grab a reference to the "LocalPlayer" from ***UWorld***, which eventually leads you to the ***AActor*** of the local player and it's camera manager. Generate a 3x4 rotation matrix that represents the camera:
```c++
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
```
Then, project needed information onto the viewport for consumption:
```c++
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
```
All this information is obtained by jumping through a list of hand-tuned memory location offsets:
```c++
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
const uintptr_t scene_component_bounds_extent_offset = 0x110;
const uintptr_t scene_component_bounds_radius_offset = 0x10c;
const uintptr_t scene_component_relative_location_offset = 0x128;
const uintptr_t scene_component_relative_rotation_offset = 0x134;
const uintptr_t scene_component_velocity_offset = 0x22c;
```
### Dependencies

- https://fmt.dev/ **Text formatting.**
- https://curl.haxx.se/ Was for downloading files but unused currently.
- http://glew.sourceforge.net/ **OpenGL extension loading.**
- https://www.glfw.org/ **OpenGL context and user input.**
- https://github.com/ocornut/imgui **The docking branch. OpenGL\GLFW3 implementation.**
