/**
 * Automobilista 2 (AMS2) session logger - saves results to a log file after each session.
 */

 // Used for memory-mapped functionality
#include <windows.h>
#include "sharedmemory.h"

#include <conio.h>

#include <string>
#include <iostream>
#include <fstream>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>

#include "nlohmann/json.hpp"

// Name of the pCars memory mapped file
#define MAP_OBJECT_NAME L"$pcars2$"

std::string GameStateToString(unsigned int gameState)
{
	switch (gameState)
	{
		case GAME_EXITED: return "GAME_EXITED";
		case GAME_FRONT_END: return "GAME_FRONT_END";
		case GAME_INGAME_PLAYING: return "GAME_INGAME_PLAYING";
		case GAME_INGAME_PAUSED: return "GAME_INGAME_PAUSED";
		case GAME_INGAME_INMENU_TIME_TICKING: return "GAME_INGAME_INMENU_TIME_TICKING";
		case GAME_INGAME_RESTARTING: return "GAME_INGAME_RESTARTING";
		case GAME_INGAME_REPLAY: return "GAME_INGAME_REPLAY";
		case GAME_FRONT_END_REPLAY: return "GAME_FRONT_END_REPLAY";
		case GAME_MAX: return "GAME_MAX";
		default: return "UNKNOWN GAME STATE";
	}
};

std::string SessionStateToString(unsigned int sessionState)
{
	switch (sessionState)
	{
		case SESSION_INVALID: return "SESSION_INVALID";
		case SESSION_PRACTICE: return "SESSION_PRACTICE";
		case SESSION_TEST: return "SESSION_TEST";
		case SESSION_QUALIFY: return "SESSION_QUALIFY";
		case SESSION_FORMATION_LAP: return "SESSION_FORMATION_LAP";
		case SESSION_RACE: return "SESSION_RACE";
		case SESSION_TIME_ATTACK: return "SESSION_TIME_ATTACK";
		case SESSION_MAX: return "SESSION_MAX";;
		default: return "UNKNOWN SESSION STATE";
	}
};

std::string RaceStateToString(unsigned int raceState)
{
	switch (raceState)
	{
		case RACESTATE_INVALID: return "RACESTATE_INVALID";
		case RACESTATE_NOT_STARTED: return "RACESTATE_NOT_STARTED";
		case RACESTATE_RACING: return "RACESTATE_RACING";
		case RACESTATE_FINISHED: return "RACESTATE_FINISHED";
		case RACESTATE_DISQUALIFIED: return "RACESTATE_DISQUALIFIED";
		case RACESTATE_RETIRED: return "RACESTATE_RETIRED";
		case RACESTATE_DNF: return "RACESTATE_DNF";
		case RACESTATE_MAX: return "RACESTATE_MAX";
		default: return "UNKNOWN RACE STATE";
	}
};

int main()
{
	HANDLE fileHandle;

	std::cout << "Waiting for shared memory connection (make sure that the game is running). Press ESC to quit..." << std::endl;
	while (true)
	{
		// Open the memory-mapped file
		fileHandle = OpenFileMapping(PAGE_READONLY, FALSE, MAP_OBJECT_NAME);
		if (fileHandle == NULL)
		{
			if (_kbhit() && _getch() == 27) // check for escape
			{
				return -1;
			}

			continue;
		}		

		break;
	}

	// Get the data structure
	const SharedMemory* sharedData = (SharedMemory*)MapViewOfFile(fileHandle, PAGE_READONLY, 0, 0, sizeof(SharedMemory));
	SharedMemory* localCopy = new SharedMemory;
	if (sharedData == NULL)
	{
		printf("Could not map view of file (%d).\n", GetLastError());

		CloseHandle(fileHandle);
		return 1;
	}

	// Ensure we're sync'd to the correct data version
	if (sharedData->mVersion != SHARED_MEMORY_VERSION)
	{
		printf( "Data version mismatch\n");
		return 1;
	}


	unsigned int updateIndex(0);
	unsigned int indexChange(0);

	bool logSaved = FALSE;

	LARGE_INTEGER lastDisplayUpdate;
	QueryPerformanceCounter(&lastDisplayUpdate);

	std::cout << "Waiting for race to end. Press ESC to quit..." << std::endl;

	while (true)
	{
		if (_kbhit() && _getch() == 27) // check for escape
		{
			break;
		}

		if (sharedData->mSequenceNumber % 2)
		{
			// Odd sequence number indicates, that write into the shared memory is just happening
			continue;
		}

		indexChange = sharedData->mSequenceNumber - updateIndex;
		updateIndex = sharedData->mSequenceNumber;

		// Copy the whole structure before processing it, otherwise the risk of the game writing into it during processing is too high.
		memcpy(localCopy,sharedData,sizeof(SharedMemory));


		if (localCopy->mSequenceNumber != updateIndex )
		{
			// More writes had happened during the read. Should be rare, but can happen.
			continue;
		}

		LARGE_INTEGER now;
		LARGE_INTEGER frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);

		double elapsedMilli = ((now.QuadPart - lastDisplayUpdate.QuadPart) * 1000.0) / frequency.QuadPart;

		// delay display update
		if (elapsedMilli < 300)
		{
			continue;
		}

		// Check if a supported session type is running
		if (localCopy->mSessionState != SESSION_RACE) continue;

		if (localCopy->mRaceState != RACESTATE_FINISHED
			&& localCopy->mRaceState != RACESTATE_DISQUALIFIED
			&& localCopy->mRaceState != RACESTATE_RETIRED
			&& localCopy->mRaceState != RACESTATE_DNF) 
		{
			if (logSaved) std::cout << "New event started. Waiting event to end..." << std::endl;
			logSaved = FALSE;
			continue;
		}

		bool stillRacing = FALSE;
		for (int i = 0; i < localCopy->mNumParticipants; i++)
		{
			if (localCopy->mRaceStates[i] != RACESTATE_FINISHED
				&& localCopy->mRaceStates[i] != RACESTATE_DISQUALIFIED
				&& localCopy->mRaceStates[i] != RACESTATE_RETIRED
				&& localCopy->mRaceStates[i] != RACESTATE_DNF)
			{
				stillRacing = TRUE;
				break;
			}
		}

		if (stillRacing) continue;
		if (logSaved) continue;

		// Save json
		nlohmann::json json;
		nlohmann::json raceInfo;
		nlohmann::json raceResult;			

		raceInfo["LapsInEvent"] = localCopy->mLapsInEvent;
		raceInfo["SessionDuration"] = localCopy->mSessionDuration;
		raceInfo["SessionAdditionalLaps"] = localCopy->mSessionAdditionalLaps;
		raceInfo["TrackLocation"] = localCopy->mTrackLocation;
		raceInfo["TrackVariation"] = localCopy->mTrackVariation;
		raceInfo["TrackLength"] = localCopy->mTrackLength;		

		for (int i = 0; i < localCopy->mNumParticipants; i++)
		{
			ParticipantInfo info = localCopy->mParticipantInfo[i];

			nlohmann::json item;
			item["Name"] = info.mName;
			item["Position"] = info.mRacePosition;
			item["BestLap"] = localCopy->mFastestLapTimes[i];
			item["RaceState"] = RaceStateToString(localCopy->mRaceStates[i]);
			item["CarName"] = localCopy->mCarNames[i];
			item["Class"] = localCopy->mCarClassNames[i];
			raceResult.push_back(item);
		}

		std::sort(raceResult.begin(), raceResult.end(), [](const nlohmann::json& a, const nlohmann::json& b)
		{
			return a["Position"] < b["Position"];
		});

		json["RaceInfo"] = raceInfo;
		json["RaceResult"] = raceResult;

		std::time_t currentTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm timeinfo;
		localtime_s(&timeinfo, &currentTime);		
		char buffer[20];
		strftime(buffer, sizeof(buffer), "%d-%m-%Y_%H-%M-%S", &timeinfo);
		std::string timeString(buffer);
		std::string fileName = "logs/data_" + timeString + ".json";

		std::ofstream file(fileName);
		file << json.dump(4);
		file.close();

		std::cout << "Log saved to " << fileName << "." << std::endl;
		
		logSaved = TRUE;

		QueryPerformanceCounter( &lastDisplayUpdate );
	}

	// Cleanup
	UnmapViewOfFile( sharedData );
	CloseHandle( fileHandle );
	delete localCopy;

	return 0;
}
