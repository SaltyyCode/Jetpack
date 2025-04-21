/*
** EPITECH PROJECT, 2025
** B-NWP-400-LIL-4-1-myftp-nicolas.dumetz
** File description:
** server
*/

#include "server/Server.hpp"
#include <algorithm>

/******************************************************************************/
/*                                                                            */
/*                               CONSTRUCTORS                                 */
/*                                                                            */
/******************************************************************************/

Jetpack::Server::Server(int port, std::string map, int expectedPlayers, bool debug)
    : _port(port), _serverSocket(-1), _numberClients(expectedPlayers), _debug(debug)
{
    try {
        this->setupServer();
        this->parseMap(map);
    } catch (const Jetpack::Error &e) {
        throw ServerError("Server setup failed: " + std::string(e.what()));
    }
}

Jetpack::Server::~Server()
{
    try {
        if (this->_serverSocket != -1)
            Jetpack::IO::close(this->_serverSocket);
    } catch (const Jetpack::IO::IOError &e) {
        Jetpack::Utils::consoleLog("Failed to close server socket: " + std::string(e.what()), Jetpack::LogInfo::ERROR);
    }
}


/******************************************************************************/
/*                                                                            */
/*                            SERVER INITIALIZATION                           */
/*                                                                            */
/******************************************************************************/

void Jetpack::Server::setupServer()
{
    Jetpack::SocketAddress addr;

    auto *in = reinterpret_cast<sockaddr_in*>(addr.raw());
    in->sin_port = Jetpack::Network::htons(this->_port);
    in->sin_addr.s_addr = INADDR_ANY;
    this->_serverSocket = Jetpack::Network::socket(AF_INET, SOCK_STREAM, 0);
    Jetpack::Network::bind(this->_serverSocket, addr.raw(), *addr.lenPtr());
    Jetpack::Network::listen(this->_serverSocket, 2);
    Jetpack::Utils::consoleLog("Server listening on port " + std::to_string(_port), Jetpack::LogInfo::INFO);
}

void Jetpack::Server::parseMap(const std::string &mapStr)
{
    std::vector<TileType> row;

    for (char c : mapStr) {
        switch (c) {
            case '\n':
                if (!row.empty()) {
                    this->_map.push_back(row);
                    row.clear();
                }
                break;
            case '_':
                row.push_back(TileType::EMPTY);
                break;
            case 'c':
                row.push_back(TileType::COIN);
                break;
            case 'e':
                row.push_back(TileType::ZAPPER);
                break;
            default:
                break;
        }
    }
    if (!row.empty())
        this->_map.push_back(row);
}


/******************************************************************************/
/*                                                                            */
/*                             CLIENT MANAGEMENT                              */
/*                                                                            */
/******************************************************************************/


void Jetpack::Server::acceptClient()
{
    Jetpack::SocketAddress clientAddr;
    int socket = Jetpack::Network::accept(this->_serverSocket, clientAddr.raw(), clientAddr.lenPtr());

    this->_clients.push_back(std::make_unique<Jetpack::RemoteClient>(socket));
    if (_debug)
        Jetpack::Utils::consoleLog("Client connected on socket fd " + std::to_string(socket), Jetpack::LogInfo::INFO);
}

void Jetpack::Server::removeClient(int index)
{
    Jetpack::Utils::consoleLog("Client disconnected.", Jetpack::LogInfo::INFO);
    this->_clients.erase(this->_clients.begin() + index);
    uint8_t waitingPlayers = static_cast<uint8_t>(this->countReadyClients());
    for (const auto& client : this->_clients) {
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), WAITING_PLAYERS_COUNT, {waitingPlayers}, this->_debug);
        if (this->_debug)
            Jetpack::Utils::consoleLog("Updated WAITING_PLAYERS_COUNT to " + std::to_string(waitingPlayers), Jetpack::LogInfo::INFO);
    }
}

/******************************************************************************/
/*                                                                            */
/*                            POLLING SYSTEM                                  */
/*                                                                            */
/******************************************************************************/

bool Jetpack::Server::waitForEvents(std::vector<struct pollfd> &pollFds) const
{
    try {
        Jetpack::Network::poll(pollFds.data(), pollFds.size(), -1);
    } catch (const Jetpack::Network::NetworkError &e) {
        Jetpack::Utils::consoleLog("Poll error: " + std::string(e.what()), Jetpack::LogInfo::ERROR);
        return false;
    }
    return true;
}

bool Jetpack::Server::waitForEvents(std::vector<struct pollfd> &pollFds, int timeoutMs)
{
    try {
        Jetpack::Network::poll(pollFds.data(), pollFds.size(), timeoutMs);
    } catch (const Jetpack::Network::NetworkError &e) {
        Jetpack::Utils::consoleLog("Poll error: " + std::string(e.what()), Jetpack::LogInfo::ERROR);
        return false;
    }
    return true;
}

void Jetpack::Server::handleNewClient(std::vector<struct pollfd> &pollFds)
{
    if (pollFds[0].revents & POLLIN) {
        try {
            this->acceptClient();
        } catch (const ServerError &e) {
            Jetpack::Utils::consoleLog(e.what(), Jetpack::LogInfo::ERROR);
        }
    }
}

void Jetpack::Server::handleClientActivity(std::vector<struct pollfd> &pollFds)
{
    size_t clientIndex;
    int client_fd;

    for (size_t i = 1; i < pollFds.size(); i++) {
        clientIndex = i - 1;
        if (clientIndex >= this->_clients.size() || !(pollFds[i].revents & POLLIN))
            continue;
        client_fd = this->_clients[clientIndex]->getSocket();
        try {
            Jetpack::Packet paquet = Jetpack::ProtocolUtils::receivePacket(client_fd, this->_debug);
            auto it = this->_packetHandlers.find(paquet.type);
            if (it != this->_packetHandlers.end()) {
                it->second(client_fd, paquet);
            } else {
                Jetpack::Utils::consoleLog("unknown Paquet", Jetpack::LogInfo::ERROR);
            }
        } catch (const std::exception &e) {
            Jetpack::Utils::consoleLog("Client disconnected (" + std::string(e.what()) + ")", Jetpack::LogInfo::ERROR);
            int playerIndex = this->findPlayerIndexByFd(client_fd);
            if (playerIndex != -1)
                _playerStates[playerIndex].setAlive(false);
            this->removeClient(clientIndex);
            this->sendGameState();
            break;
        }
    }
}

int Jetpack::Server::findPlayerIndexByFd(int fd) const
{
    for (size_t i = 0; i < _playerStates.size(); ++i)
        if (_playerStates[i].getSocket() == fd)
            return static_cast<int>(i);
    return -1;
}


std::vector<struct pollfd> Jetpack::Server::preparePollFds() const
{
    std::vector<struct pollfd> fds;
    fds.push_back({this->_serverSocket, POLLIN, 0});
    for (const auto &client : this->_clients)
        fds.push_back({client->getSocket(), POLLIN, 0});
    return fds;
}

/******************************************************************************/
/*                                                                            */
/*                               SERVER LOOP                                  */
/*                                                                            */
/******************************************************************************/

void Jetpack::Server::run()
{
    while (true) {
        auto pollFds = this->preparePollFds();
        if (!this->waitForEvents(pollFds))
            continue;
        this->handleNewClient(pollFds);
        this->handleClientActivity(pollFds);
        if (this->countReadyClients() == this->_numberClients) {
            this->startGameLoop();
            break;
        }
    }
    Jetpack::Utils::consoleLog("game is over", Jetpack::LogInfo::INFO);
}

/******************************************************************************/
/*                                                                            */
/*                               PROTOCOLE                                    */
/*                                                                            */
/******************************************************************************/

int Jetpack::Server::findClientIndexByFd(int fd) const
{
    for (size_t i = 0; i < this->_clients.size(); i++)
        if (this->_clients[i]->getSocket() == fd)
            return static_cast<int>(i);
    return -1;
}

void Jetpack::Server::lunchStart()
{
    for (const auto &client : this->_clients)
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), GAME_START, {}, this->_debug);
    Jetpack::Utils::consoleLog("All Clients are Ready. GAME_START send.", Jetpack::LogInfo::SUCCESS);
    this->_playerStates.clear();
    for (const auto &client : this->_clients) {
        PlayerState player(client->getId(), client->getSocket());
        player.setX(0);
        player.setY(static_cast<int>(_map.size()) - 1);
        player.map = _map;
        this->_playerStates.push_back(player);
    }
}

int Jetpack::Server::countReadyClients() const
{
    int count = 0;

    for (auto &client : this->_clients)
        if (client->isReady())
            count++;
    return count;
}

void Jetpack::Server::handleLogin(int fd, const Jetpack::Packet&)
{
    int index = this->findClientIndexByFd(fd);
    uint8_t id;
    uint8_t waitingPlayers;

    if (index == -1)
        return;
    id = static_cast<uint8_t>(index);
    this->_clients[index]->setId(id);
    this->_clients[index]->setReady(true);
    Jetpack::ProtocolUtils::sendPacket(fd, LOGIN_RESPONSE, {id, 0, static_cast<uint8_t>(this->_numberClients)}, this->_debug);
    Jetpack::Utils::consoleLog("New Client accepted, has ID " + std::to_string(id), Jetpack::LogInfo::SUCCESS);
    if (_debug)
        Jetpack::Utils::consoleLog("Sent LOGIN_RESPONSE to fd " + std::to_string(fd), Jetpack::LogInfo::INFO);
    this->sendMap(id, this->_map);
    waitingPlayers = static_cast<uint8_t>(this->countReadyClients());
    for (const auto &client : this->_clients) {
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), WAITING_PLAYERS_COUNT, {waitingPlayers}, this->_debug);
        if (_debug)
            Jetpack::Utils::consoleLog("Sent WAITING_PLAYERS_COUNT = " + std::to_string(waitingPlayers), Jetpack::LogInfo::INFO);
    }
    if (this->countReadyClients() == this->_numberClients)
        this->lunchStart();
    else
        Jetpack::Utils::consoleLog("Client ID " + std::to_string(id) + " is waiting...", Jetpack::LogInfo::INFO);
}

void Jetpack::Server::handlePlayerAction(int fd, const Jetpack::Packet& paquet)
{
    if (paquet.payload.empty())
        return;
    uint8_t actionRaw = paquet.payload[0];
    Jetpack::PlayerActionType action = static_cast<Jetpack::PlayerActionType>(actionRaw);
    if (action != Jetpack::PlayerActionType::JUMP)
        return;
    if (this->_debug)
        Jetpack::Utils::consoleLog("Received PLAYER_ACTION (JUMP) from fd " + std::to_string(fd), Jetpack::LogInfo::INFO);
    for (auto &player : this->_playerStates) {
        if (player.getSocket() == fd && player.isAlive()) {
            if (player.getY() > 0.0f) {
                player.setY(player.getY() - JETPACK_JUMP_SPEED * TICK_INTERVAL);
                player.setHasJumped(true);
                if (this->_debug)
                    Jetpack::Utils::consoleLog("Applied jump for player " + std::to_string(player.getId()), Jetpack::LogInfo::INFO);
            }
            break;
        }
    }
    std::vector<uint8_t> ackPayload = {PLAYER_ACTION};
    Jetpack::ProtocolUtils::sendPacket(fd, ACTION_ACK, ackPayload, this->_debug);
    if (this->_debug)
        Jetpack::Utils::consoleLog("Sent ACTION_ACK to fd " + std::to_string(fd), Jetpack::LogInfo::INFO);
}


void Jetpack::Server::sendCoinEvent(uint8_t playerId, int coinX, int coinY)
{
    std::vector<uint8_t> payload = {playerId};
    uint8_t xBytes[sizeof(coinX)];
    uint8_t yBytes[sizeof(coinY)];
    std::memcpy(xBytes, &coinX, sizeof(coinX));
    std::memcpy(yBytes, &coinY, sizeof(coinY));
    payload.insert(payload.end(), xBytes, xBytes + sizeof(coinX));
    payload.insert(payload.end(), yBytes, yBytes + sizeof(coinY));
    if (this->_debug)
        Jetpack::Utils::consoleLog("Sending COIN_EVENT: player " + std::to_string(playerId) +
            " collected coin at (" + std::to_string(coinX) + "," + std::to_string(coinY) + ")", Jetpack::LogInfo::INFO);
    for (const auto &client : this->_clients)
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), COIN_EVENT, payload, this->_debug);
}

void Jetpack::Server::sendGameState()
{
    std::vector<uint8_t> payload;

    for (const auto &player : this->_playerStates) {
        payload.push_back(player.getId());
        payload.push_back(player.isAlive() ? 1 : 0);
    }
    if (this->_debug)
        Jetpack::Utils::consoleLog("Sending GAME_STATE to all clients", Jetpack::LogInfo::INFO);
    for (const auto &client : this->_clients)
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), GAME_STATE, payload, this->_debug);
}

void Jetpack::Server::sendPositionUpdate(uint8_t playerId, float x, float y)
{
    std::vector<uint8_t> payload;
    uint8_t xBytes[sizeof(x)];
    uint8_t yBytes[sizeof(y)];

    if (playerId == INVALID_ID)
        return;
    payload.push_back(playerId);
    std::memcpy(xBytes, &x, sizeof(x));
    payload.insert(payload.end(), xBytes, xBytes + sizeof(x));
    std::memcpy(yBytes, &y, sizeof(y));
    payload.insert(payload.end(), yBytes, yBytes + sizeof(y));
    if (this->_debug)
        Jetpack::Utils::consoleLog("Sending POSITION_UPDATE for player " + std::to_string(playerId) +
            " -> x: " + std::to_string(x) + ", y: " + std::to_string(y), Jetpack::LogInfo::INFO);
    for (const auto &client : this->_clients)
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), POSITION_UPDATE, payload, this->_debug);
}

void Jetpack::Server::sendMap(int playerId, const std::vector<std::vector<TileType>> &map)
{
    std::vector<uint8_t> payload;

    payload.push_back(playerId);
    for (const auto &row : map) {
        for (TileType tile : row) {
            payload.push_back(static_cast<uint8_t>(tile));
        }
        payload.push_back('\n');
    }
    if (this->_debug)
        Jetpack::Utils::consoleLog("Sending MAP_TRANSFER to all clients for player " + std::to_string(playerId), Jetpack::LogInfo::INFO);
    for (const auto &client : this->_clients)
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), MAP_TRANSFER, payload, this->_debug);
}


/******************************************************************************/
/*                                                                            */
/*                               GAME                                         */
/*                                                                            */
/******************************************************************************/

void Jetpack::Server::startGameLoop()
{
    auto lastTime = std::chrono::steady_clock::now();
    float accumulator = 0.0f;
    const int mapHeight = static_cast<int>(_map.size());
    const int mapWidth = static_cast<int>(_map[0].size());
    bool gameRunning = true;

    while (gameRunning) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        accumulator += dt;
        this->processNetworkEvents();
        while (accumulator >= TICK_INTERVAL) {
            accumulator -= TICK_INTERVAL;
            this->processPlayers(mapHeight, mapWidth);
        }
        if (this->countAlivePlayers() <= 1) {
            gameRunning = false;
            break;
        }
        gameRunning = this->isGameStillRunning();
    }
    this->handleGameOver();
}

int Jetpack::Server::countAlivePlayers() const
{
    int count = 0;
    for (const auto &player : this->_playerStates) {
        if (player.isAlive())
            count++;
    }
    return count;
}

void Jetpack::Server::processNetworkEvents()
{
    auto pollFds = this->preparePollFds();

    if (this->waitForEvents(pollFds, 0))
        this->handleClientActivity(pollFds);
}

void Jetpack::Server::processPlayers(int mapHeight, int mapWidth)
{
    for (auto &player : this->_playerStates) {
        if (!player.isAlive())
            continue;
        player.setX(player.getX() + SCROLL_SPEED * TICK_INTERVAL);
        if (!player.hasJumped() && player.getY() < mapHeight - 1)
            player.setY(player.getY() + GRAVITY_SPEED * TICK_INTERVAL);
        player.setHasJumped(false);
        if (player.getTileX() >= mapWidth && !player.hasFinished()) {
            player.setFinished(true);
            if (_debug)
                Jetpack::Utils::consoleLog("Player " + std::to_string(player.getId()) + " finished the map.", Jetpack::LogInfo::INFO);
            this->sendGameState();
            continue;
        }
        this->checkCollisions(player);
        this->sendPositionUpdate(player.getId(), player.getX(), player.getY());
    }
}

void Jetpack::Server::checkCollisions(PlayerState &player)
{
    int tileXLeft = static_cast<int>(std::floor(player.getX() + COLLISION_MARGIN_LEFT));
    int tileXRight = static_cast<int>(std::floor(player.getX() + 1.0f - COLLISION_MARGIN_RIGHT));
    int tileYTop = static_cast<int>(std::floor(player.getY() + COLLISION_MARGIN_TOP));
    int tileYBottom = static_cast<int>(std::floor(player.getY() + 1.0f - COLLISION_MARGIN_BOTTOM));

    for (int y = tileYTop; y <= tileYBottom; ++y) {
        for (int x = tileXLeft; x <= tileXRight; ++x) {
            if (y < 0 || y >= static_cast<int>(_map.size()) ||
                x < 0 || x >= static_cast<int>(_map[0].size()))
                continue;
            TileType tile = _map[y][x];
            if (tile == TileType::COIN) {
                bool alreadyCollected = false;
                for (const auto& coinPos : player.getCoinCollected()) {
                    if (coinPos.first == x && coinPos.second == y) {
                        alreadyCollected = true;
                        break;
                    }
                }
                if (!alreadyCollected) {
                    player.addCoin();
                    player.addCoinCollected(x, y);
                    sendCoinEvent(player.getId(), x, y);
                }
            } else if (tile == TileType::ZAPPER) {
                player.setAlive(false);
                this->sendGameState();
                return;
            }
        }
    }
}


bool Jetpack::Server::isGameStillRunning()
{
    for (const auto& p : _playerStates)
        if (!p.hasFinished() && p.isAlive())
            return true;
    return false;
}

void Jetpack::Server::handleGameOver()
{
    uint8_t winnerId = INVALID_ID;
    int bestScore = -1;
    bool tie = false;
    int aliveCount = 0;
    int lastAliveId = INVALID_ID;

    if (this->_debug)
        Jetpack::Utils::consoleLog("Computing GAME_OVER winner...", Jetpack::LogInfo::INFO);
    for (const auto &player : this->_playerStates) {
        if (player.isAlive()) {
            aliveCount++;
            lastAliveId = player.getId();
        }
    }
    if (aliveCount == 1) {
        winnerId = lastAliveId;
        if (this->_debug)
            Jetpack::Utils::consoleLog("Only one player alive: Player " + std::to_string(winnerId), Jetpack::LogInfo::INFO);
    } else {
        for (const auto &player : this->_playerStates) {
            if (!player.isAlive() && !player.hasFinished())
                continue;
            if (this->_debug)
                Jetpack::Utils::consoleLog("Alive Player " + std::to_string(player.getId()) + " has " + std::to_string(player.getCoins()) + " coins.", Jetpack::LogInfo::INFO);
            if (player.getCoins() > bestScore) {
                bestScore = player.getCoins();
                winnerId = player.getId();
                tie = false;
            } else if (player.getCoins() == bestScore)
                tie = true;
        }
        if (tie) {
            winnerId = 255;
            if (this->_debug)
                Jetpack::Utils::consoleLog("It's a tie among alive players. No unique winner.", Jetpack::LogInfo::INFO);
        } else {
            if (this->_debug)
                Jetpack::Utils::consoleLog("Winner among alive players is player " + std::to_string(winnerId), Jetpack::LogInfo::INFO);
        }
    }
    std::vector<uint8_t> payload = {winnerId};
    for (const auto &client : _clients) {
        Jetpack::ProtocolUtils::sendPacket(client->getSocket(), GAME_OVER, payload, this->_debug);
        if (this->_debug)
            Jetpack::Utils::consoleLog("Sent GAME_OVER to fd " + std::to_string(client->getSocket()), Jetpack::LogInfo::INFO);
    }
    Jetpack::Utils::consoleLog("Game ended (all players dead or map finished)", Jetpack::LogInfo::INFO);
}
