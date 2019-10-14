#ifdef CONF_RPC

#include <fstream>
#include <google/protobuf/util/delimited_message_util.h>

#include <base/math.h>
#include <engine/shared/config.h>
#include "../gamemodes/DDRace.h"
#include "../gamecontroller.h"
#include "../save.h"

#include "rpc_score.h"


std::vector<std::function<bool(CRPCScore*)>> CRPCScore::ms_GlobalPendingRequests;

CRPCScore::CRPCScore(CGameContext* pGameServer) :
m_pGameServer(pGameServer),
m_pServer(pGameServer->Server()),
m_pRPC(m_pServer->RPC())
{
	str_copy(m_aMap, g_Config.m_SvMap, sizeof(m_aMap));
	FormatUuid(m_pGameServer->GameUuid(), m_aGameUuid, sizeof(m_aGameUuid));

	auto MapName(std::make_shared<db::MapName>());
	MapName->set_name(m_aMap);
	auto Fut = RPC()->BestTime(MapName);
	AddPendingRequest(
	[this, Fut]()
	{
		if (!Fut.Ready())
			return false;

		try
		{
			GameServer()->m_pController->m_CurrentRecord = Fut.Get().time();
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() != grpc::StatusCode::NOT_FOUND)
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);

}


void CRPCScore::Process()
{
	for (size_t i = 0; i < ms_GlobalPendingRequests.size(); ++i)
	{
		if (ms_GlobalPendingRequests[i](this))
			ms_GlobalPendingRequests.erase(ms_GlobalPendingRequests.begin() + i);
	}

	for (size_t i = 0; i < m_PendingRequests.size(); ++i)
	{
		if (m_PendingRequests[i]())
			m_PendingRequests.erase(m_PendingRequests.begin() + i);
	}
}


void CRPCScore::OnShutdown()
{
	for (size_t i = 0; i < ms_GlobalPendingRequests.size(); ++i)
		ms_GlobalPendingRequests[i](nullptr);
}


void CRPCScore::CheckBirthday(int ClientID)
{
	auto PlayerName = std::make_shared<db::PlayerName>();
	std::string Name (Server()->ClientName(ClientID));
	PlayerName->set_name(Name);
	auto Fut = RPC()->CheckBirthDay(PlayerName);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, ClientID, Name, Fut, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			int YearsAgo = Fut.Get().years_ago();
			if (YearsAgo == 0)
				return true;
			char aBuf[512];

			str_format(aBuf, sizeof(aBuf), "Happy DDNet birthday to %s for finishing their first map %d year%s ago!", Name.c_str(), YearsAgo, YearsAgo > 1 ? "s" : "");
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			str_format(aBuf, sizeof(aBuf), "Happy DDNet birthday, %s!\nYou have finished your first map exactly %d year%s ago!", Name.c_str(), YearsAgo, YearsAgo > 1 ? "s" : "");
			GameServer()->SendBroadcast(aBuf, ClientID);

		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::LoadScore(int ClientID)
{
	std::string PlayerName (Server()->ClientName(ClientID));
	std::string MapName (m_aMap);
	auto PaM(std::make_shared<db::PlayerAndMap>());
	PaM->set_player_name(PlayerName);
	PaM->set_map_name(MapName);
	auto Fut = RPC()->GetPlayerScore(PaM);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;

		if (!Fut.Ready())
			return false;

		try
		{
			auto Score = Fut.Get();
			PlayerData(ClientID)->m_BestTime = Score.time();
			PlayerData(ClientID)->m_CurrentTime = Score.time();
			GameServer()->m_apPlayers[ClientID]->m_Score = -Score.time();
			GameServer()->m_apPlayers[ClientID]->m_HasFinishScore = true;

			if(g_Config.m_SvCheckpointSave)
			{
				for(int i = 0; i < minimum(static_cast<int>(NUM_CHECKPOINTS), Score.check_point_size()); ++i)
				{
					PlayerData(ClientID)->m_aBestCpTime[i] = Score.check_point(i);
				}
			}
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() != grpc::StatusCode::NOT_FOUND)
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::MapInfo(int ClientID, const char* MapName)
{
	auto Map(std::make_shared<db::MapName>());
	std::string MapStr(MapName);
	Map->set_name(MapStr);
	auto Fut = RPC()->MapInfo(Map);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, Fut, ClientID, MapStr, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;
		try
		{
			if (!Fut.Ready())
				return false;
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "No map like \"%s\" found.", MapStr.c_str());
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::MapVote(std::shared_ptr<CMapVoteResult> *ppResult, int ClientID, const char* MapName)
{

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientID];
	if (!pPlayer)
		return;

	int64 Now = Server()->Tick();
	int Timeleft = pPlayer->m_LastVoteCall + Server()->TickSpeed()*g_Config.m_SvVoteDelay - Now;

	if (Now < pPlayer->m_FirstVoteTick)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "You must wait %d seconds before making your first vote", (int)((pPlayer->m_FirstVoteTick - Now) / Server()->TickSpeed()) + 1);
		GameServer()->SendChatTarget(ClientID, aBuf);
		return;
	}
	if (pPlayer->m_LastVoteCall && Timeleft > 0)
	{
		char aChatmsg[512] = {0};
		str_format(aChatmsg, sizeof(aChatmsg), "You must wait %d seconds before making another vote", (Timeleft/Server()->TickSpeed())+1);
		GameServer()->SendChatTarget(ClientID, aChatmsg);
		return;
	}
	if(time_get() < GameServer()->m_LastMapVote + (time_freq() * g_Config.m_SvVoteMapTimeDelay))
	{
		char chatmsg[512] = {0};
		str_format(chatmsg, sizeof(chatmsg), "There's a %d second delay between map-votes, please wait %d seconds.", g_Config.m_SvVoteMapTimeDelay, (int)(((GameServer()->m_LastMapVote+(g_Config.m_SvVoteMapTimeDelay * time_freq()))/time_freq())-(time_get()/time_freq())));
		GameServer()->SendChatTarget(ClientID, chatmsg);
	}

	auto Map(std::make_shared<db::MapName>());
	Map->set_name(MapName);
	std::string MapStr(MapName);
	auto Fut = RPC()->FindMap(Map);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, Fut, ClientID, MapStr, JoinTime]()
	{
		if (Server()->ClientJoinTick(ClientID) != JoinTime)
			return true;

		if (!Fut.Ready())
			return false;

		try
		{
			auto MapInfo = Fut.Get();
			GameServer()->StartMapVote(MapInfo.map_name().c_str(), MapInfo.server_type().c_str(), ClientID);
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				char aBuf[512];
				str_format(aBuf, sizeof(aBuf), "No map like \"%s\" found. Try adding a '%%' at the start if you don't know the first character. Example: /map %%castle for \"Out of Castle\"", MapStr.c_str());
				GameServer()->SendChatTarget(ClientID, aBuf);
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::OnFinish(unsigned int Size, int* pClientID, float Time, const char *pTimestamp, float* apCpTime[NUM_CHECKPOINTS], bool Team, bool NotEligible)
{
	auto Finish(std::make_shared<db::Finish>());
	Finish->set_map_name(m_aMap);
	Finish->set_game_uuid(m_aGameUuid);
	Finish->set_team(Team);
	Finish->set_time(Time);
	std::map<int, int> JoinTimes;

	for (unsigned int i = 0; i < Size; ++i)
	{
		JoinTimes[pClientID[i]] = Server()->ClientJoinTick(pClientID[i]);
		db::TeeFinish* Tee = Finish->add_tee_finished();
		Tee->set_player_name(Server()->ClientName(pClientID[i]));

		for(int j = 0; j < NUM_CHECKPOINTS; j++)
			Tee->set_check_point(j, apCpTime[i][j]);
	}
	auto Fut = RPC()->OnFinish(Finish);
	AddPendingRequest(
	[this, Fut, Finish, JoinTimes]()
	{
		if (!Fut.Ready())
			return false;

		try
		{
			auto Chat = Fut.Get();
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, Chat.chat_all().c_str());
			for (auto& Msg : Chat.chat_id())
			{
				if (JoinTimes.find(Msg.first) == JoinTimes.end())
					continue;
				if (Server()->ClientJoinTick(Msg.first) == JoinTimes.at(Msg.first))
					GameServer()->SendChatTarget(Msg.first, Msg.second.c_str());
			}
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}

		return true;
	}
	);
}


void CRPCScore::ShowRank(int ClientID, const char* pName, bool Search)
{
	std::string PlayerName(pName);
	std::string RequestingPlayerName(Server()->ClientName(ClientID));
	auto PaM(std::make_shared<db::PlayerAndMap>());
	PaM->set_map_name(m_aMap);
	PaM->set_player_name(pName);
	auto Fut = RPC()->ShowRank(PaM);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, Fut, ClientID, JoinTime, PlayerName, RequestingPlayerName]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			auto Msg = Fut.Get();
			if(g_Config.m_SvHideScore)
			{
				GameServer()->SendChatTarget(ClientID, Msg.text().c_str());
			}
			else
			{
				char aBuf[512];
				str_format(aBuf, sizeof(aBuf), "%s\n(requested by %s)", Msg.text().c_str(), RequestingPlayerName.c_str());
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			}

		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowTeamRank(int ClientID, const char* pName, bool Search)
{
	auto PaM(std::make_shared<db::PlayerAndMap>());
	PaM->set_map_name(m_aMap);
	PaM->set_player_name(pName);
	std::string PlayerName(pName);
	std::string RequestingPlayerName(Server()->ClientName(ClientID));
	int JoinTime = Server()->ClientJoinTick(ClientID);
	auto Fut = RPC()->ShowTeamRank(PaM);
	AddPendingRequest(
	[this, Fut, RequestingPlayerName, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			auto Msg = Fut.Get();

			if(g_Config.m_SvHideScore)
			{
				GameServer()->SendChatTarget(ClientID, Msg.text().c_str());
			}
			else
			{
				char aBuf[2400];
				str_format(aBuf, sizeof(aBuf), "%s\n(requested by %s)", Msg.text().c_str(), RequestingPlayerName.c_str());
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			}
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);

}


void CRPCScore::ShowTimes(int ClientID, const char* pName, int Debut)
{
	auto PaM(std::make_shared<db::PlayerAndMap>());
	PaM->set_map_name(m_aMap);
	PaM->set_player_name(pName);
	auto Fut = RPC()->ShowTimes(PaM);
	int JoinTime = Server()->ClientJoinTick(ClientID);
	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;

		try
		{
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowTimes(int ClientID, int Debut)
{
	ShowTimes(ClientID, "", Debut);
}


void CRPCScore::ShowTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut)
{
	auto RankRequest(std::make_shared<db::TopRankRequest>());
	RankRequest->set_map_name(m_aMap);
	RankRequest->set_num_ranks(5);
	RankRequest->set_offset(Debut);
	auto Fut = RPC()->ShowTop(RankRequest);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowTeamTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut)
{
	auto RankRequest(std::make_shared<db::TopRankRequest>());
	RankRequest->set_map_name(m_aMap);
	RankRequest->set_num_ranks(5);
	RankRequest->set_offset(Debut);
	auto Fut = RPC()->ShowTop(RankRequest);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;
		try
		{
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowPoints(int ClientID, const char* pName, bool Search)
{
	auto PlayerName(std::make_shared<db::PlayerName>());
	PlayerName->set_name(pName);
	auto Fut = RPC()->ShowPoints(PlayerName);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;

		try
		{
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::ShowTopPoints(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut)
{
	auto TopRequest(std::make_shared<db::TopPointsRequest>());
	TopRequest->set_num_ranks(5);
	TopRequest->set_offset(Debut);
	auto Fut = RPC()->ShowTopPoints(TopRequest);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;

		try
		{
			GameServer()->SendChatTarget(ClientID, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::RandomMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars)
{
	auto MapRequest(std::make_shared<db::RandomMapRequest>());
	MapRequest->set_stars(stars);
	MapRequest->set_current_map(m_aMap);
	MapRequest->set_server_type(g_Config.m_SvServerType);
	auto Fut = RPC()->GetRandomMap(MapRequest);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime, MapRequest]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;

		try
		{
			std::string MapName = Fut.Get().name();
			GameServer()->StartMapVote(MapName.c_str(), MapRequest->server_type().c_str(), ClientID);
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				GameServer()->m_LastMapVote = 0;
				if (JoinTime == Server()->ClientJoinTick(ClientID))
					GameServer()->SendChatTarget(ClientID, "No maps found on this server!");
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::RandomUnfinishedMap(std::shared_ptr<CRandomMapResult> *ppResult, int ClientID, int stars)
{
	auto MapRequest(std::make_shared<db::RandomUnfinishedMapRequest>());
	MapRequest->set_stars(stars);
	MapRequest->set_current_map(m_aMap);
	MapRequest->set_server_type(g_Config.m_SvServerType);
	MapRequest->set_player_name(Server()->ClientName(ClientID));
	auto Fut = RPC()->GetRandomUnfinishedMap(MapRequest);

	int JoinTime = Server()->ClientJoinTick(ClientID);

	AddPendingRequest(
	[this, Fut, ClientID, JoinTime, MapRequest]()
	{
		if (JoinTime != Server()->ClientJoinTick(ClientID))
			return true;
		if (!Fut.Ready())
			return false;

		try
		{
			std::string MapName = Fut.Get().name();
			GameServer()->StartMapVote(MapName.c_str(), MapRequest->server_type().c_str(), ClientID);
		}
		catch (DatabaseException& Ex)
		{
			if (Ex.Status().error_code() == grpc::StatusCode::NOT_FOUND)
			{
				GameServer()->m_LastMapVote = 0;
				if (JoinTime == Server()->ClientJoinTick(ClientID))
					GameServer()->SendChatTarget(ClientID, "You have no more unfinished maps on this server!");
			}
			else
				dbg_msg("rpcscore", "%s", Ex.what());
		}
		return true;
	}
	);
}


void CRPCScore::SaveTeam(int Team, const char* Code, int ClientID, const char* Server)
{
	int Num = -1;

	CSaveTeam SavedTeam(GameServer()->m_pController);
	if(
		(g_Config.m_SvTeam == 3 || (Team > 0 && Team < MAX_CLIENTS)) &&
		((CGameControllerDDRace*)(GameServer()->m_pController))->m_Teams.Count(Team) > 0
	)
	{
		Num = SavedTeam.save(Team);
		switch (Num)
		{
			case 1:
				GameServer()->SendChatTarget(ClientID, "You have to be in a team (from 1-63)!");
				break;
			case 2:
				GameServer()->SendChatTarget(ClientID, "Could not find your Team!");
				break;
			case 3:
				GameServer()->SendChatTarget(ClientID, "Unable to find all Characters!");
				break;
			case 4:
				GameServer()->SendChatTarget(ClientID, "Your team hast not started yet!");
				break;
		}
		if(Num)
			return;
	}
	else
	{
		GameServer()->SendChatTarget(ClientID, "You have to be in a team (from 1-63)");
		return;
	}

	if (Num)
		return;

	auto SaveProtobuf(std::make_shared<db::TeamSave>());
	SaveProtobuf->set_code(Code);
	SaveProtobuf->set_map_name(m_aMap);
	SavedTeam.FillProtobuf(*SaveProtobuf);
	((CGameControllerDDRace*)(GameServer()->m_pController))->m_Teams.KillSavedTeam(Team);

	auto Fut = RPC()->SaveTeam(SaveProtobuf);
	CUuid GameUUID = GameServer()->GameUuid();
	AddGlobalPendingRequest(
	[Fut, Team, SaveProtobuf, GameUUID](CRPCScore* pScore)
	{
		if (pScore == nullptr)
		{
			try
			{
				Fut.Get();
			}
			catch (DatabaseException& Ex)
			{
				std::ofstream File(g_Config.m_SvRPCFailureFile, std::ofstream::app);
				google::protobuf::util::SerializeDelimitedToOstream(*SaveProtobuf, &File);
			}
			return true;
		}

		if (!Fut.Ready())
			return false;
		try
		{
			std::string Msg = Fut.Get().text();
			// TODO: Jointicks...
			if (GameUUID == pScore->GameServer()->GameUuid())
				pScore->GameServer()->SendChatTeam(Team, Fut.Get().text().c_str());
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
			std::ofstream File(g_Config.m_SvRPCFailureFile, std::ofstream::app);
			google::protobuf::util::SerializeDelimitedToOstream(*SaveProtobuf, &File);
		}

		return true;
	}
	);
}


void CRPCScore::LoadTeam(const char* Code, int ClientID)
{
	auto LoadRequest(std::make_shared<db::TeamLoadRequest>());
	LoadRequest->set_code(Code);
	LoadRequest->set_map_name(m_aMap);
	auto Fut = RPC()->LoadTeam(LoadRequest);
	CUuid GameUUID = GameServer()->GameUuid();
	AddGlobalPendingRequest(
	[Fut, ClientID, GameUUID](CRPCScore* pScore)
	{
		if (pScore == nullptr)
		{
			try
			{
				auto Save = Fut.Get();
				std::ofstream File(g_Config.m_SvRPCFailureFile, std::ofstream::app);
				google::protobuf::util::SerializeDelimitedToOstream(Save, &File);
			}
			catch (DatabaseException& Ex)
			{
				dbg_msg("rpcscore", "%s", Ex.what());
			}
			return true;
		}

		if (!Fut.Ready())
			return false;

		if (GameUUID != pScore->GameServer()->GameUuid())
		{
			auto Save = std::make_shared<db::TeamSave>(Fut.Get());
			auto Fut2 = pScore->RPC()->SaveTeam(Save);
			pScore->AddGlobalPendingRequest(
			[Fut2, Save](CRPCScore* pScore)
			{
				if (pScore == nullptr)
				{
					try
					{
						Fut2.Get();
					}
					catch (DatabaseException& Ex)
					{
						std::ofstream File(g_Config.m_SvRPCFailureFile, std::ofstream::app);
						google::protobuf::util::SerializeDelimitedToOstream(*Save, &File);
					}
					return true;
				}

				if (!Fut2.Ready())
					return false;
				try
				{
					std::string Msg = Fut2.Get().text();
					// TODO: Jointicks...
				}
				catch (DatabaseException& Ex)
				{
					dbg_msg("rpcscore", "%s", Ex.what());
				}

				return true;
			}
			);
		}

		try
		{
			auto TeamSave = Fut.Get();

			CSaveTeam SavedTeam(pScore->GameServer()->m_pController);

			int Num = SavedTeam.LoadProtobuf(TeamSave);

			if(Num)
				pScore->GameServer()->SendChatTarget(ClientID, "Unable to load savegame: data corrupted");
			else
			{

				bool Found = false;
				for (int i = 0; i < SavedTeam.GetMembersCount(); i++)
				{
					if(str_comp(SavedTeam.m_pSavedTees[i].GetName(), pScore->Server()->ClientName(ClientID)) == 0)
					{
						Found = true;
						break;
					}
				}
				if(!Found)
					pScore->GameServer()->SendChatTarget(ClientID, "You don't belong to this team");
				else
				{
					int Team = ((CGameControllerDDRace*)(pScore->GameServer()->m_pController))->m_Teams.m_Core.Team(ClientID);

					Num = SavedTeam.load(Team);

					if(Num == 1)
					{
						pScore->GameServer()->SendChatTarget(ClientID, "You have to be in a team (from 1-63)");
					}
					else if(Num == 2)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "Too many players in this team, should be %d", SavedTeam.GetMembersCount());
						pScore->GameServer()->SendChatTarget(ClientID, aBuf);
					}
					else if(Num >= 10 && Num < 100)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "Unable to find player: '%s'", SavedTeam.m_pSavedTees[Num-10].GetName());
						pScore->GameServer()->SendChatTarget(ClientID, aBuf);
					}
					else if(Num >= 100 && Num < 200)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "%s is racing right now, Team can't be loaded if a Tee is racing already", SavedTeam.m_pSavedTees[Num-100].GetName());
						pScore->GameServer()->SendChatTarget(ClientID, aBuf);
					}
					else if(Num >= 200)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "Everyone has to be in a team, %s is in team 0 or the wrong team", SavedTeam.m_pSavedTees[Num-200].GetName());
						pScore->GameServer()->SendChatTarget(ClientID, aBuf);
					}
					else
						pScore->GameServer()->SendChatTeam(Team, "Loading successfully done");
				}
			}
		}
		catch (DatabaseException& Ex)
		{
			dbg_msg("rpcscore", "%s", Ex.what());
		}

		return true;
	}
	);
}

#endif
