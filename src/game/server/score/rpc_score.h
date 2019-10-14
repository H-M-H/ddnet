#pragma once

#include <forward_list>
#include <functional>

#include <engine/server.h>
#include <engine/server/rpc/database_client.h>

#include "../score.h"
#include "../player.h"

class CRPCScore : public IScore
{

public:
	CRPCScore(CGameContext *pGameServer);

	// TODO: Get rid of these:
	virtual void SaveScore(int ClientID, float Time, const char *pTimestamp, float aCpTime[NUM_CHECKPOINTS], bool NotEligible) {}
	virtual void SaveTeamScore(int *pClientIDs, unsigned int Size, float Time, const char *pTimestamp) {}

	virtual void CheckBirthday(int ClientID);
	virtual void LoadScore(int ClientID);
	virtual void MapInfo(int ClientID, const char* MapName);
	virtual void MapVote(std::shared_ptr<CMapVoteResult> *ppResult, int ClientID, const char* MapName);
	virtual void OnFinish(unsigned int Size, int* ClientID, float Time, const char *pTimestamp,
			float* CpTime[NUM_CHECKPOINTS], bool Team, bool NotEligible);
	virtual void ShowRank(int ClientID, const char* pName, bool Search = false);
	virtual void ShowTeamRank(int ClientID, const char* pName, bool Search = false);
	virtual void ShowTimes(int ClientID, const char* pName, int Debut = 1);
	virtual void ShowTimes(int ClientID, int Debut = 1);
	virtual void ShowTop5(IConsole::IResult *pResult, int ClientID,
			void *pUserData, int Debut = 1);
	virtual void ShowTeamTop5(IConsole::IResult *pResult, int ClientID,
			void *pUserData, int Debut = 1);
	virtual void ShowPoints(int ClientID, const char* pName, bool Search = false);
	virtual void ShowTopPoints(IConsole::IResult *pResult, int ClientID,
			void *pUserData, int Debut = 1);
	virtual void RandomMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars);
	virtual void RandomUnfinishedMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars);
	virtual void SaveTeam(int Team, const char* Code, int ClientID, const char* Server);
	virtual void LoadTeam(const char* Code, int ClientID);

	virtual void Process();

	virtual void OnShutdown();

private:
	CGameContext *GameServer() { return m_pGameServer; }
	IServer *Server() { return m_pServer; }
	CDatabaseClient *RPC() {return m_pRPC; }

	/* Take a callable without arguments returning a bool and add it to a
	 * list of callables to be processed.
	 *
	 * The callables will be called each tick and removed from the list if
	 * evaluating to true.
	 * The idea is to use lambdas to capture the context after an RPC-Call,
	 * especially the returned CFuture object, and if the future is ready
	 * mutate the state of the game in the main thread.
	 */
	template<typename T>
	void AddPendingRequest(T&& Func)
	{
		m_PendingRequests.emplace_front(std::forward<T>(Func));
	}

	CGameContext *m_pGameServer;
	IServer *m_pServer;

	char m_aMap[64];
	char m_aGameUuid[UUID_MAXSTRSIZE];

	std::forward_list<std::function<bool()>> m_PendingRequests;

	CDatabaseClient *m_pRPC;

};
