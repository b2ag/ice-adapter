#include "Testserver.h"

#include "TcpSession.h"
#include "logging.h"

namespace faf {

Testserver::Testserver():
  mServer(54321, false),
  mCurrentPlayerId(0)
{
  mServer.connectionChanged.connect([this](TcpSession* socket, ConnectionState c)
  {
    if (c == ConnectionState::Connected)
    {
      FAF_LOG_TRACE << "login " << mCurrentPlayerId;
      mPlayerSockets.insert(std::make_pair(mCurrentPlayerId, socket));
      mSocketPlayers.insert(std::make_pair(socket, mCurrentPlayerId));
      mPlayersLogins.insert(std::make_pair(mCurrentPlayerId, std::string("Player") + std::to_string(mCurrentPlayerId)));

      Json::Value params(Json::arrayValue);
      params.append(mCurrentPlayerId);
      mServer.sendRequest("onLogin",
                          params,
                          socket);
      ++mCurrentPlayerId;
      sendGamelist(socket);
    }
    else
    {
      auto it = mSocketPlayers.find(socket);
      if (it != mSocketPlayers.end())
      {
        PlayerIdType leavingPlayerId = it->second;
        FAF_LOG_TRACE << "logout " << leavingPlayerId;
        disconnectPlayerFromGame(leavingPlayerId);
        mPlayerSockets.erase(leavingPlayerId);
        mSocketPlayers.erase(it);
        mPlayersLogins.erase(leavingPlayerId);
      }
    }
  });

  mServer.setRpcCallback("hostGame",
                         [this](Json::Value const& paramsArray,
                         Json::Value & result,
                         Json::Value & error,
                         Socket* socket)
  {
    auto it = mSocketPlayers.find(socket);
    if (it != mSocketPlayers.end())
    {
      PlayerIdType hostingPlayerId = it->second;
      disconnectPlayerFromGame(hostingPlayerId);
      mGames[hostingPlayerId].insert(hostingPlayerId);
      mPlayersGames[hostingPlayerId] = hostingPlayerId;
      {
        Json::Value params(Json::arrayValue);
        params.append("hostGame");
        Json::Value hostGameParams(Json::arrayValue);
        hostGameParams.append("testmap");
        mServer.sendRequest("sendToIceAdapter",
                            params,
                            socket);
      }
      sendGamelist();
    }
  });

  mServer.setRpcCallback("leaveGame",
                         [this](Json::Value const& paramsArray,
                         Json::Value & result,
                         Json::Value & error,
                         Socket* socket)
  {
    auto it = mSocketPlayers.find(socket);
    if (it != mSocketPlayers.end())
    {
      disconnectPlayerFromGame(it->second);
    }
  });

  mServer.setRpcCallback("joinGame",
                         [this](Json::Value const& paramsArray,
                         Json::Value & result,
                         Json::Value & error,
                         Socket* joiningPlayerSocket)
  {
    if (paramsArray.size() < 1)
    {
      error = "Need 1 parameters: remotePlayerId (int)";
      return;
    }
    auto joiningPlayerIt = mSocketPlayers.find(joiningPlayerSocket);
    if (joiningPlayerIt == mSocketPlayers.end())
    {
      error = "playerId not found";
      return;
    }

    PlayerIdType joiningPlayerId = joiningPlayerIt->second;
    disconnectPlayerFromGame(joiningPlayerId);

    /* Send "connectToPeer" to game host */
    PlayerIdType hostingPlayerId = paramsArray[0].asInt();
    auto gameIt = mGames.find(hostingPlayerId);
    if (gameIt == mGames.end())
    {
      error = "game not found";
      return;
    }
    gameIt->second.insert(joiningPlayerId);
    mPlayersGames[joiningPlayerId] = hostingPlayerId;

    /* send "connectToPeer" to host */
    {
      Json::Value params(Json::arrayValue);
      params.append("connectToPeer");
      Json::Value connectToPeerParams(Json::arrayValue);
      connectToPeerParams.append(std::string("Player") + std::to_string(joiningPlayerId));
      connectToPeerParams.append(joiningPlayerId);
      connectToPeerParams.append(true);
      params.append(connectToPeerParams);
      mServer.sendRequest("sendToIceAdapter",
                          params,
                          mPlayerSockets[hostingPlayerId]);
    }

    /* Send "joinGame" to joining player */
    Json::Value joinGamesParams(Json::arrayValue);
    Json::Value joinGamesParamsParams(Json::arrayValue);
    joinGamesParamsParams.append(mPlayersLogins[hostingPlayerId]);
    joinGamesParamsParams.append(hostingPlayerId);
    joinGamesParams.append("joinGame");
    joinGamesParams.append(joinGamesParamsParams);
    mServer.sendRequest("sendToIceAdapter",
                        joinGamesParams,
                        joiningPlayerSocket);
    for(PlayerIdType const& existingPlayerId: gameIt->second)
    {
      if (existingPlayerId == joiningPlayerId)
      {
        continue;
      }
      if (existingPlayerId == hostingPlayerId)
      {
        continue;
      }
      /* Send "connectToPeer" to fellow players to connect to joining player */
      {
        Json::Value params(Json::arrayValue);
        params.append("connectToPeer");
        Json::Value connectToPeerParams(Json::arrayValue);
        connectToPeerParams.append(std::string("Player") + std::to_string(joiningPlayerId));
        connectToPeerParams.append(joiningPlayerId);
        connectToPeerParams.append(true);
        params.append(connectToPeerParams);
        mServer.sendRequest("sendToIceAdapter",
                            params,
                            mPlayerSockets[existingPlayerId]);
      }
      /* Send "connectToPeer" to joining player to connect to fellow players   */
      {
        Json::Value params(Json::arrayValue);
        params.append("connectToPeer");
        Json::Value connectToPeerParams(Json::arrayValue);
        connectToPeerParams.append(std::string("Player") + std::to_string(existingPlayerId));
        connectToPeerParams.append(existingPlayerId);
        connectToPeerParams.append(false);
        params.append(connectToPeerParams);
        mServer.sendRequest("sendToIceAdapter",
                            params,
                            joiningPlayerSocket);
      }
    }
  });
  mServer.setRpcCallback("sendSdpMessage",
                         [this](Json::Value const& paramsArray,
                         Json::Value & result,
                         Json::Value & error,
                         Socket* socket)
  {
    if (paramsArray.size() < 4)
    {
      error = "Need 2 parameters: playerId (int), remotePlayerId (int), type (string), message (string)";
      return;
    }
    auto remoteId = paramsArray[1].asInt();
    auto remoteIt = mPlayerSockets.find(remoteId);
    if (remoteIt == mPlayerSockets.end())
    {
      error = "remotePlayerId not found";
      return;
    }
    Json::Value params(Json::arrayValue);
    params.append("addSdpMessage");
    Json::Value setSdpParams(Json::arrayValue);
    setSdpParams.append(mSocketPlayers[socket]);
    setSdpParams.append(paramsArray[2]);
    setSdpParams.append(paramsArray[3]);
    params.append(setSdpParams);
    mServer.sendRequest("sendToIceAdapter",
                        params,
                        remoteIt->second);
  });
}

void Testserver::sendGamelist(Socket* s)
{
  Json::Value params(Json::arrayValue);
  Json::Value gameObject(Json::objectValue);
  for (auto it = mGames.cbegin(), end = mGames.cend(); it != end; ++it)
  {
    gameObject[mPlayersLogins[it->first]] = it->first;
  }
  params.append(gameObject);
  mServer.sendRequest("onGamelist",
                      params,
                      s);
}

void Testserver::disconnectPlayerFromGame(PlayerIdType leavingPlayerId)
{
  /* first check if the player hosts a game */
  auto hostingGameIt = mGames.find(leavingPlayerId);
  if (hostingGameIt != mGames.end())
  {
    for(PlayerIdType const& fellowPlayerId: hostingGameIt->second)
    {
      Json::Value params(Json::arrayValue);
      params.append(leavingPlayerId);
      mServer.sendRequest("onHostLeft",
                          params,
                          mPlayerSockets[fellowPlayerId]);

    }
    mGames.erase(hostingGameIt);
    sendGamelist();
  }
  auto leavinPlayersGameIt = mPlayersGames.find(leavingPlayerId);
  if (leavinPlayersGameIt != mPlayersGames.end())
  {
    auto gameId = leavinPlayersGameIt->second;
    auto gameIt = mGames.find(gameId);
    if (gameIt != mGames.end())
    {
      /* disconnect all fellow players from the game */
      for(PlayerIdType const& fellowPlayerId: gameIt->second)
      {
        if (fellowPlayerId != leavingPlayerId)
        {
          {
            Json::Value disconnectParams(Json::arrayValue);
            disconnectParams.append(leavingPlayerId);
            Json::Value params(Json::arrayValue);
            params.append("disconnectFromPeer");
            params.append(disconnectParams);
            mServer.sendRequest("sendToIceAdapter",
                                params,
                                mPlayerSockets[fellowPlayerId]);
          }
          {
            Json::Value disconnectParams(Json::arrayValue);
            disconnectParams.append(fellowPlayerId);
            Json::Value params(Json::arrayValue);
            params.append("disconnectFromPeer");
            params.append(disconnectParams);
            mServer.sendRequest("sendToIceAdapter",
                                params,
                                mPlayerSockets[leavingPlayerId]);
          }
        }
      }
      gameIt->second.erase(leavingPlayerId);
    }
    mPlayersGames.erase(leavinPlayersGameIt);
  }
}

} // namespace faf

namespace sigc {
  SIGC_FUNCTORS_DEDUCE_RESULT_TYPE_WITH_DECLTYPE
}

int main(int argc, char *argv[])
{
  try
  {
    Gio::init();

    faf::logging_init();

    auto loop = Glib::MainLoop::create();

    faf::Testserver server;
    loop->run();
  }
  catch (const Gio::Error& e)
  {
    FAF_LOG_ERROR << "Glib error: " << e.what();
    return 1;
  }
  catch (const std::exception& e)
  {
    FAF_LOG_ERROR << "error: " << e.what();
    return 1;
  }
  catch (...)
  {
    FAF_LOG_ERROR << "unknown error occured";
    return 1;
  }

  return 0;
}
