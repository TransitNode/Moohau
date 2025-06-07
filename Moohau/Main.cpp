#include <Windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <conio.h>

#include "SDK.hpp"
#include "SDK/Basic.hpp"
#include "SDK/Basic.cpp"
#include "SDK/Mordhau_classes.hpp"
#include "SDK/Engine_functions.cpp"
#include "SDK/CoreUObject_functions.cpp"

#include "SDK/Mordhau_functions.cpp"
#include "SDK/BP_MordhauCharacter_functions.cpp"
#include "SDK/BP_MordhauPlayerController_functions.cpp"
#include "SDK/BP_MordhauEquipmentPart_functions.cpp"

// console stuff
HANDLE g_ConsoleHandle = NULL;
HANDLE g_ConsoleInput = NULL;

// command handling
std::string g_CommandBuffer = "";
bool g_CommandMode = false;
bool g_ShowHelp = false;

// hotkey tracking
bool g_VKeyPreviousState = false;
HANDLE g_HotkeyThread = NULL;
HANDLE g_StateManagementThread = NULL;
CRITICAL_SECTION g_StateCriticalSection;

// block collider override state
bool g_BlockColliderForceDisabled = false;

void ConsoleOutput(const std::string& text)
{
	if (g_ConsoleHandle)
	{
		DWORD written;
		WriteConsoleA(g_ConsoleHandle, text.c_str(), text.length(), &written, NULL);
	}
}

void ClearConsole()
{
	COORD coordScreen = { 0, 0 };
	DWORD cCharsWritten;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD dwConSize;

	GetConsoleScreenBufferInfo(g_ConsoleHandle, &csbi);
	dwConSize = csbi.dwSize.X * csbi.dwSize.Y;
	FillConsoleOutputCharacterA(g_ConsoleHandle, ' ', dwConSize, coordScreen, &cCharsWritten);
	SetConsoleCursorPosition(g_ConsoleHandle, coordScreen);
}

SDK::AAdvancedCharacter* GetLocalCharacter()
{
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world || !world->OwningGameInstance || world->OwningGameInstance->LocalPlayers.Num() == 0)
		return nullptr;

	SDK::ULocalPlayer* localPlayer = world->OwningGameInstance->LocalPlayers[0];
	if (!localPlayer || !localPlayer->PlayerController || !localPlayer->PlayerController->Pawn)
		return nullptr;

	return static_cast<SDK::AAdvancedCharacter*>(localPlayer->PlayerController->Pawn);
}

// hotkey thread - polls for V key presses
DWORD WINAPI HotkeyThread(LPVOID lpParam)
{
	while (true)
	{
		bool vKeyCurrentState = (GetKeyState(0x56) & 0x8000) != 0;

		// check for key press transition
		if (vKeyCurrentState && !g_VKeyPreviousState)
		{
			SDK::AAdvancedCharacter* character = GetLocalCharacter();
			if (character)
			{
				SDK::AMordhauCharacter* mordhauCharacter = static_cast<SDK::AMordhauCharacter*>(character);
				if (mordhauCharacter && mordhauCharacter->IsA(SDK::AMordhauCharacter::StaticClass()))
				{
					SDK::FVector currentLocation = mordhauCharacter->K2_GetActorLocation();
					mordhauCharacter->RequestClimb(currentLocation, false);
					ConsoleOutput("[CLIMB] Requested climb\n");
				}
			}
		}

		g_VKeyPreviousState = vKeyCurrentState;
		Sleep(50);
	}
	return 0;
}

// background thread to handle block collider state
DWORD WINAPI StateManagementThread(LPVOID lpParam)
{
	while (true)
	{
		EnterCriticalSection(&g_StateCriticalSection);
		bool shouldDisable = g_BlockColliderForceDisabled;
		LeaveCriticalSection(&g_StateCriticalSection);

		if (shouldDisable)
		{
			SDK::AAdvancedCharacter* character = GetLocalCharacter();
			if (character)
			{
				SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(character);
				if (mordhauChar && mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
				{
					if (mordhauChar->BlockCollider)
					{
						mordhauChar->DisableBlockCollider();
					}
				}
			}
		}

		Sleep(200); // check every 200ms
	}
	return 0;
}

void CommandEnableBlockCollider(const std::vector<std::string>& args)
{
	EnterCriticalSection(&g_StateCriticalSection);
	g_BlockColliderForceDisabled = false;
	LeaveCriticalSection(&g_StateCriticalSection);

	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("Error: Character not found\n");
		return;
	}

	SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(character);
	if (!mordhauChar || !mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
	{
		ConsoleOutput("Error: Invalid character type\n");
		return;
	}

	mordhauChar->EnableBlockCollider();
	ConsoleOutput("Block collider enabled\n");
}

void CommandDisableBlockCollider(const std::vector<std::string>& args)
{
	EnterCriticalSection(&g_StateCriticalSection);
	g_BlockColliderForceDisabled = true;
	LeaveCriticalSection(&g_StateCriticalSection);

	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("Error: Character not found\n");
		return;
	}

	SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(character);
	if (!mordhauChar || !mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
	{
		ConsoleOutput("Error: Invalid character type\n");
		return;
	}

	mordhauChar->DisableBlockCollider();
	ConsoleOutput("Block collider disabled (permanent)\n");
}

// teleport all available weapons to player
void CommandTeleportWeapons(const std::vector<std::string>& args)
{
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world || !world->PersistentLevel)
	{
		ConsoleOutput("Error: World access failed\n");
		return;
	}

	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("Error: Character not found\n");
		return;
	}

	SDK::FVector playerLoc = character->K2_GetActorLocation();
	SDK::TArray<SDK::AActor*>& actors = world->PersistentLevel->Actors;

	int weaponCount = 0;
	ConsoleOutput("Searching for weapons...\n");

	for (int i = 0; i < actors.Num(); i++)
	{
		SDK::AActor* actor = actors[i];
		if (!actor) continue;

		SDK::AMordhauEquipment* equipment = static_cast<SDK::AMordhauEquipment*>(actor);
		if (equipment && equipment->IsA(SDK::AMordhauEquipment::StaticClass()))
		{
			// only grab unowned weapons
			if (!equipment->BelongsToCharacter())
			{
				equipment->K2_SetActorLocation(playerLoc, false, nullptr, true);
				weaponCount++;
			}
		}
	}

	ConsoleOutput("Teleported " + std::to_string(weaponCount) + " weapons\n");
}

void CommandShowCharacterInfo(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("Error: Character not found\n");
		return;
	}

	ConsoleOutput("\n--- CHARACTER INFO ---\n");
	ConsoleOutput("Health: " + std::to_string(character->Health) + "/100\n");
	ConsoleOutput("Team: " + std::to_string(character->Team) + "\n");
	ConsoleOutput("Dead: " + std::string(character->bIsDead ? "Yes" : "No") + "\n");

	SDK::FVector loc = character->K2_GetActorLocation();
	ConsoleOutput("Position: " + std::to_string(loc.X) + ", " + std::to_string(loc.Y) + ", " + std::to_string(loc.Z) + "\n");

	// mordhau specific stuff
	SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(character);
	if (mordhauChar && mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
	{
		ConsoleOutput("Stamina: " + std::to_string(mordhauChar->Stamina) + "/100\n");

		if (mordhauChar->Motion)
		{
			ConsoleOutput("Motion: " + mordhauChar->Motion->GetFullName() + "\n");
			ConsoleOutput("Movement restriction: " + std::to_string((int)mordhauChar->Motion->MovementRestriction) + "\n");
			ConsoleOutput("Speed factor: " + std::to_string(mordhauChar->Motion->SpeedFactor) + "\n");
		}

		ConsoleOutput("Block collider: " + std::string(mordhauChar->BlockCollider ? "Active" : "Inactive") + "\n");
		ConsoleOutput("First person: " + std::string(mordhauChar->bIsFirstPerson ? "Yes" : "No") + "\n");
	}
}

void CommandListPlayers(const std::vector<std::string>& args)
{
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (!world || !world->GameState)
	{
		ConsoleOutput("Error: Game state unavailable\n");
		return;
	}

	ConsoleOutput("\n--- PLAYERS ---\n");
	for (int i = 0; i < world->GameState->PlayerArray.Num(); i++)
	{
		SDK::APlayerState* ps = world->GameState->PlayerArray[i];
		if (ps)
		{
			ConsoleOutput("Player " + std::to_string(i + 1) + ":\n");
			ConsoleOutput("  ID: " + std::to_string(ps->PlayerId) + "\n");
			ConsoleOutput("  Score: " + std::to_string(ps->Score) + "\n");

			if (ps->PawnPrivate)
			{
				SDK::AAdvancedCharacter* playerChar = static_cast<SDK::AAdvancedCharacter*>(ps->PawnPrivate);
				if (playerChar)
				{
					ConsoleOutput("  HP: " + std::to_string(playerChar->Health) + "/100\n");
					ConsoleOutput("  Team: " + std::to_string(playerChar->Team) + "\n");
					ConsoleOutput("  Status: " + std::string(playerChar->bIsDead ? "Dead" : "Alive") + "\n");
				}
			}
			ConsoleOutput("\n");
		}
	}
}

void CommandTrip(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("No character found\n");
		return;
	}

	character->RequestTrip();
	ConsoleOutput("Trip executed\n");
}

void CommandClimb(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("No character found\n");
		return;
	}

	SDK::AMordhauCharacter* mordhauCharacter = static_cast<SDK::AMordhauCharacter*>(character);
	if (!mordhauCharacter || !mordhauCharacter->IsA(SDK::AMordhauCharacter::StaticClass()))
	{
		ConsoleOutput("Character type mismatch\n");
		return;
	}

	SDK::FVector currentLoc = mordhauCharacter->K2_GetActorLocation();
	mordhauCharacter->RequestClimb(currentLoc, false);
	ConsoleOutput("Climb executed\n");
}

void CommandRagdoll(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("No character found\n");
		return;
	}

	character->StartRagdoll();
	ConsoleOutput("Ragdoll activated\n");
}

void CommandKnockback(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("No character found\n");
		return;
	}

	float x = 500.0f, y = 0.0f, z = 200.0f; // defaults

	// parse args if provided
	if (args.size() >= 4)
	{
		try
		{
			x = std::stof(args[1]);
			y = std::stof(args[2]);
			z = std::stof(args[3]);
		}
		catch (...)
		{
			ConsoleOutput("Invalid values, using defaults\n");
		}
	}

	SDK::FVector knockbackVec(x, y, z);
	bool result = character->Knockback(knockbackVec);

	ConsoleOutput("Knockback: " + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "\n");
	ConsoleOutput("Result: " + std::string(result ? "OK" : "Failed") + "\n");
}

void CommandExplodeLimbs(const std::vector<std::string>& args)
{
	SDK::AAdvancedCharacter* character = GetLocalCharacter();
	if (!character)
	{
		ConsoleOutput("No character found\n");
		return;
	}

	SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(character);
	if (!mordhauChar || !mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
	{
		ConsoleOutput("Character type mismatch\n");
		return;
	}

	float x = 1000.0f, y = 0.0f, z = 500.0f; // explosion defaults

	if (args.size() >= 4)
	{
		try
		{
			x = std::stof(args[1]);
			y = std::stof(args[2]);
			z = std::stof(args[3]);
		}
		catch (...)
		{
			ConsoleOutput("Invalid values, using defaults\n");
		}
	}

	SDK::FVector explosionForce(x, y, z);

	// get controller ref
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	SDK::AController* ctrl = nullptr;
	if (world && world->OwningGameInstance && world->OwningGameInstance->LocalPlayers.Num() > 0)
	{
		SDK::ULocalPlayer* localPlayer = world->OwningGameInstance->LocalPlayers[0];
		if (localPlayer)
		{
			ctrl = localPlayer->PlayerController;
		}
	}

	mordhauChar->ExplodeLimbs(explosionForce, mordhauChar, ctrl);
	ConsoleOutput("Limbs exploded: " + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + "\n");
}

void CommandHelp(const std::vector<std::string>& args)
{
	ConsoleOutput("\n--- COMMANDS ---\n");
	ConsoleOutput("help                    - This help\n");
	ConsoleOutput("clear                   - Clear screen\n");
	ConsoleOutput("diagnostics (diag)      - System info\n");
	ConsoleOutput("charinfo                - Character stats\n");
	ConsoleOutput("listplayers             - Show all players\n");
	ConsoleOutput("teleportweapons         - Grab all weapons\n");
	ConsoleOutput("trip                    - Trip character\n");
	ConsoleOutput("climb                   - Climb at location\n");
	ConsoleOutput("enableblock             - Enable block collider\n");
	ConsoleOutput("disableblock            - Disable block collider\n");
	ConsoleOutput("ragdoll                 - Enable ragdoll\n");
	ConsoleOutput("knockback [x y z]       - Knockback with force\n");
	ConsoleOutput("explodelimbs [x y z]    - Explode limbs with force\n");
	ConsoleOutput("exit                    - Quit\n");
	ConsoleOutput("\n--- HOTKEYS ---\n");
	ConsoleOutput("V                       - Quick climb\n");
	ConsoleOutput("\nType commands and press ENTER\n");
	ConsoleOutput("-------------------\n\n");
}

void CommandDiagnostics(const std::vector<std::string>& args)
{
	ConsoleOutput("=== Moohau Diagnostics ===\n\n");

	// world check
	ConsoleOutput("1. WORLD ACCESS:\n");
	SDK::UWorld* world = SDK::UWorld::GetWorld();
	if (world)
	{
		ConsoleOutput("   [OK] UWorld: " + world->GetFullName() + "\n");
	}
	else
	{
		ConsoleOutput("   [FAIL] UWorld is NULL\n");
		return;
	}

	// player info
	ConsoleOutput("\n2. PLAYER DATA:\n");
	if (world->OwningGameInstance && world->OwningGameInstance->LocalPlayers.Num() > 0)
	{
		SDK::ULocalPlayer* localPlayer = world->OwningGameInstance->LocalPlayers[0];
		if (localPlayer && localPlayer->PlayerController)
		{
			ConsoleOutput("   [OK] PlayerController: " + localPlayer->PlayerController->GetFullName() + "\n");

			SDK::APlayerState* playerState = localPlayer->PlayerController->PlayerState;
			if (playerState)
			{
				ConsoleOutput("   [OK] PlayerState: " + playerState->GetFullName() + "\n");
				ConsoleOutput("   Player ID: " + std::to_string(playerState->PlayerId) + "\n");
				ConsoleOutput("   Score: " + std::to_string(playerState->Score) + "\n");
			}

			SDK::APawn* pawn = localPlayer->PlayerController->Pawn;
			if (pawn)
			{
				ConsoleOutput("   [OK] Pawn: " + pawn->GetFullName() + "\n");

				SDK::AAdvancedCharacter* advChar = static_cast<SDK::AAdvancedCharacter*>(pawn);
				if (advChar)
				{
					ConsoleOutput("   Health: " + std::to_string(advChar->Health) + "/100\n");
					ConsoleOutput("   Team: " + std::to_string(advChar->Team) + "\n");
					ConsoleOutput("   Dead: " + std::string(advChar->bIsDead ? "Yes" : "No") + "\n");

					SDK::AMordhauCharacter* mordhauChar = static_cast<SDK::AMordhauCharacter*>(advChar);
					if (mordhauChar && mordhauChar->IsA(SDK::AMordhauCharacter::StaticClass()))
					{
						ConsoleOutput("   [OK] MordhauCharacter found\n");
						ConsoleOutput("   Stamina: " + std::to_string(mordhauChar->Stamina) + "/100\n");

						if (mordhauChar->Motion)
						{
							ConsoleOutput("   Motion: " + mordhauChar->Motion->GetFullName() + "\n");
							ConsoleOutput("   Movement restriction: " + std::to_string((int)mordhauChar->Motion->MovementRestriction) + "\n");
							ConsoleOutput("   Speed: " + std::to_string(mordhauChar->Motion->SpeedFactor) + "\n");
							ConsoleOutput("   Can attack: " + std::string(mordhauChar->Motion->bCanAttack ? "Yes" : "No") + "\n");
							ConsoleOutput("   Can block: " + std::string(mordhauChar->Motion->bCanBlock ? "Yes" : "No") + "\n");
						}

						ConsoleOutput("   Block collider: " + std::string(mordhauChar->BlockCollider ? "Active" : "Inactive") + "\n");
						ConsoleOutput("   First person: " + std::string(mordhauChar->bIsFirstPerson ? "Yes" : "No") + "\n");
					}
				}
			}
		}
	}

	// mod status
	ConsoleOutput("\n3. MOD STATE:\n");
	EnterCriticalSection(&g_StateCriticalSection);
	bool blockDisabled = g_BlockColliderForceDisabled;
	LeaveCriticalSection(&g_StateCriticalSection);

	ConsoleOutput("   Block override: " + std::string(blockDisabled ? "DISABLED" : "NORMAL") + "\n");

	// network info
	ConsoleOutput("\n4. NETWORK:\n");
	SDK::UNetDriver* netDriver = world->NetDriver;
	if (netDriver)
	{
		ConsoleOutput("   [OK] NetDriver: " + netDriver->GetFullName() + "\n");
		ConsoleOutput("   Type: " + netDriver->NetDriverName.ToString() + "\n");

		if (netDriver->ServerConnection)
		{
			SDK::UNetConnection* serverConn = netDriver->ServerConnection;
			ConsoleOutput("   [OK] Server connection: " + serverConn->GetFullName() + "\n");
			ConsoleOutput("   Last receive: " + std::to_string(serverConn->LastReceiveTime) + "\n");
			ConsoleOutput("   Max packet: " + std::to_string(serverConn->MaxPacket) + "\n");
		}

		ConsoleOutput("   Max client rate: " + std::to_string(netDriver->MaxClientRate) + "\n");
		ConsoleOutput("   Timeout: " + std::to_string(netDriver->ConnectionTimeout) + "\n");
	}
	else
	{
		ConsoleOutput("   [FAIL] NetDriver unavailable\n");
	}

	ConsoleOutput("=== End diagnostics ===\n\n");
}

void ExecuteCommand(const std::string& commandLine)
{
	if (commandLine.empty()) return;

	std::vector<std::string> tokens;
	std::stringstream ss(commandLine);
	std::string token;

	while (ss >> token)
	{
		tokens.push_back(token);
	}

	if (tokens.empty()) return;

	std::string cmd = tokens[0];
	std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

	if (cmd == "help")
	{
		CommandHelp(tokens);
	}
	else if (cmd == "clear")
	{
		ClearConsole();
	}
	else if (cmd == "diagnostics" || cmd == "diag")
	{
		CommandDiagnostics(tokens);
	}
	else if (cmd == "charinfo")
	{
		CommandShowCharacterInfo(tokens);
	}
	else if (cmd == "listplayers")
	{
		CommandListPlayers(tokens);
	}
	else if (cmd == "teleportweapons")
	{
		CommandTeleportWeapons(tokens);
	}
	else if (cmd == "trip")
	{
		CommandTrip(tokens);
	}
	else if (cmd == "climb")
	{
		CommandClimb(tokens);
	}
	else if (cmd == "enableblock")
	{
		CommandEnableBlockCollider(tokens);
	}
	else if (cmd == "disableblock")
	{
		CommandDisableBlockCollider(tokens);
	}
	else if (cmd == "ragdoll")
	{
		CommandRagdoll(tokens);
	}
	else if (cmd == "knockback")
	{
		CommandKnockback(tokens);
	}
	else if (cmd == "explodelimbs")
	{
		CommandExplodeLimbs(tokens);
	}
	else if (cmd == "exit")
	{
		ExitProcess(0);
	}
	else
	{
		ConsoleOutput("Unknown: " + cmd + " (type 'help')\n");
	}
}

bool CheckConsoleInput()
{
	INPUT_RECORD inputRecord;
	DWORD eventsRead;

	if (PeekConsoleInput(g_ConsoleInput, &inputRecord, 1, &eventsRead) && eventsRead > 0)
	{
		if (ReadConsoleInput(g_ConsoleInput, &inputRecord, 1, &eventsRead))
		{
			if (inputRecord.EventType == KEY_EVENT && inputRecord.Event.KeyEvent.bKeyDown)
			{
				char keyChar = inputRecord.Event.KeyEvent.uChar.AsciiChar;

				if (keyChar == '\r') // enter
				{
					ConsoleOutput("\n> " + g_CommandBuffer + "\n");
					ExecuteCommand(g_CommandBuffer);
					g_CommandBuffer.clear();
					return true;
				}
				else if (keyChar == '\b') // backspace
				{
					if (!g_CommandBuffer.empty())
					{
						g_CommandBuffer.pop_back();
						ConsoleOutput("\b \b");
					}
				}
				else if (keyChar >= 32 && keyChar <= 126) // printable chars
				{
					g_CommandBuffer += keyChar;
					ConsoleOutput(std::string(1, keyChar));
				}
			}
		}
	}
	return false;
}

DWORD MainThread(HMODULE module)
{
	AllocConsole();

	g_ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	g_ConsoleInput = GetStdHandle(STD_INPUT_HANDLE);

	SetConsoleTitleA("Moohau 1.0");

	InitializeCriticalSection(&g_StateCriticalSection);

	// start threads
	g_HotkeyThread = CreateThread(0, 0, HotkeyThread, NULL, 0, 0);
	g_StateManagementThread = CreateThread(0, 0, StateManagementThread, NULL, 0, 0);

	// console input setup
	DWORD mode;
	GetConsoleMode(g_ConsoleInput, &mode);
	mode &= ~ENABLE_LINE_INPUT;
	mode |= ENABLE_PROCESSED_INPUT;
	SetConsoleMode(g_ConsoleInput, mode);

	// startup message
	ConsoleOutput("=== Moohau ===\n");
	ConsoleOutput("Physics & Fun Commands\n");
	ConsoleOutput("Type 'help' for commands\n");
	ConsoleOutput("V key = quick climb\n\n");

	// main input loop
	while (true)
	{
		CheckConsoleInput();
		Sleep(100);
	}

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	case DLL_PROCESS_DETACH:
		// cleanup
		if (g_HotkeyThread)
		{
			TerminateThread(g_HotkeyThread, 0);
			CloseHandle(g_HotkeyThread);
		}
		if (g_StateManagementThread)
		{
			TerminateThread(g_StateManagementThread, 0);
			CloseHandle(g_StateManagementThread);
		}

		DeleteCriticalSection(&g_StateCriticalSection);

		if (g_ConsoleHandle)
		{
			FreeConsole();
		}
		break;
	}

	return TRUE;
}