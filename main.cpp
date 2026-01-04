#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <random>

#include "json.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

const int INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;

using nlohmann::json;

using namespace std;

long long nowMs() {
    auto now = chrono::steady_clock::now();
    return chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count();
}

struct Player {
    int id;
    string name;
    int totalScore;
    bool isHost;
    bool answered;
    int socketFd;
};

struct Question {
    int id;
    string text;
    vector<string> answers;
    int correct;
    int timeLimitMs;
};

struct Answer {
    int playerId;
    int answerIndex;
    long long receiveTimeMs;
};

enum class GameState {
    NO_GAME,
    SETUP,
    LOBBY,
    QUESTION_ACTIVE,
    QUESTION_RESULTS,
    FINISHED
};

struct Game {
    string roomCode;
    GameState state = GameState::NO_GAME;
    map<int, Player> players;
    vector<Question> questions;
    int nextPlayerId = 1;
    int currentQuestionIndex = -1;
    long long questionStartTime = 0;
    vector<Answer> answers;
};


// helper do znajdowania id gracza po fd gniazda
int findPlayerIdByFd(const Game &game, int fd) {
    for (const auto &p : game.players) {
        if (p.second.socketFd == fd) {
            return p.first;
        }
    }
    return -1;
}

// helper do wysylania jsona przez socket
void sendJson(int client, const json &j) {
    string msg = j.dump() + "\n";
    send(client, msg.c_str(), (int)msg.size(), 0);
}

// helper do rozgłaszania jsona do wszystkich graczy
void broadcast(const Game &game, const json &j) {
    for (const auto &p : game.players) {
        sendJson(p.second.socketFd, j);
    }
}

// START - buildery wiadomosci wysylanych przez serwer 

json jsonLobby(const Game &game) {
    json j;
    j["type"] = "lobby_update";
    vector<string> names;
    for (const auto &p : game.players) {
        if (!p.second.isHost) {
            names.push_back(p.second.name);
        }
    }
    j["players"] = names;
    j["room"] = game.roomCode;
    return j;
}

json jsonQuestion(const Question &q) {
    json j;
    j["type"] = "question";
    j["question_id"] = q.id;
    j["text"] = q.text;
    j["answers"] = q.answers;
    j["time_limit_ms"] = q.timeLimitMs;
    return j;
}

json jsonQuestionResults(const Game &game, const Question &q) {
    json res = json::array();
    for (const auto &a : game.answers) {
        const auto &player = game.players.at(a.playerId);
        int points = 0;
        if (a.answerIndex == q.correct) {
            long long delta = a.receiveTimeMs - game.questionStartTime;
            points = max(0, 1000 - (int)(delta / 10));
        }
        json item;
        item["name"] = player.name;
        item["points"] = points;
        item["total"] = player.totalScore;
        res.push_back(item);
    }
    json j;
    j["type"] = "question_results";
    j["correct"] = q.correct;
    j["results"] = res;
    return j;
}

json jsonFinal(const Game &game) {
    vector<Player> ranking;
    for (const auto &p : game.players) {
        if (!p.second.isHost) {
            ranking.push_back(p.second);
        }
    }
    sort(ranking.begin(), ranking.end(), [](const Player &a, const Player &b) {
        return a.totalScore > b.totalScore;
    });
    json res = json::array();
    for (const auto &p : ranking) {
        json item;
        item["name"] = p.name;
        item["total"] = p.totalScore;
        res.push_back(item);
    }
    json j;
    j["type"] = "final_results";
    j["ranking"] = res;
    return j;
}

// END - buildery wiadomosci wysylanych przez serwer 

// prosta funkcja do generowania 4 cyfrowego room id, zakladamy jedna gre na raz wiec nie potrzebujemy sprawdzac "kolizji"
string generateRoomCode() {
    int code = 1000 + rand() % 9000;
    return to_string(code);
}

void resetAll(Game &game) {
    game.roomCode.clear();
    game.state = GameState::NO_GAME;
    game.players.clear();
    game.questions.clear();
    game.answers.clear();
    game.nextPlayerId = 1;
    game.currentQuestionIndex = -1;
    game.questionStartTime = 0;
}

// nalicza punkty za odpowiedzi zebrane w game.answers
void scoreAnswers(Game &game, const Question &q) {
    for (auto &a : game.answers) {
        auto &player = game.players[a.playerId];
        if (a.answerIndex == q.correct) {
            long long delta = a.receiveTimeMs - game.questionStartTime;
            int points = max(0, 1000 - (int)(delta / 10));
            player.totalScore += points;
        }
    }
}

// ustawia flagi answered=false u wszystkich graczy
void resetAnswerFlags(Game &game) {
    for (auto &p : game.players) {
        p.second.answered = false;
    }
}

// uruchamia nastepne pytanie i rozglasza je do graczy
void startQuestion(Game &game) {
    if (game.currentQuestionIndex + 1 >= (int)game.questions.size()) {
        return;
    }
    game.currentQuestionIndex++;
    const auto &q = game.questions[game.currentQuestionIndex];
    game.answers.clear();
    resetAnswerFlags(game);
    game.questionStartTime = nowMs();
    game.state = GameState::QUESTION_ACTIVE;
    broadcast(game, jsonQuestion(q));
}

// konczy gre i rozglasza finalny ranking
void finishGame(Game &game) {
    game.state = GameState::FINISHED;
    broadcast(game, jsonFinal(game));
}

// reset gry - pelny reset
void resetGame(Game &game) {
    resetAll(game);
}

// zamyka klienta i usuwa go z listy graczy
void closeClient(Game &game, int clientFd) {
    for (auto it = game.players.begin(); it != game.players.end(); ) {
        if (it->second.socketFd == clientFd) {
            it = game.players.erase(it);
        } else {
            ++it;
        }
    }
    close(clientFd);
    // jesli jest lobby to rozglos aktualna liste graczy
    if (game.state == GameState::LOBBY) {
        broadcast(game, jsonLobby(game));
    }
}

bool handleJoin(Game &game, int clientFd, const json &msg) {
    // check czy gra w lobby i istnieje
    if (game.state != GameState::LOBBY) {
        json err;
        err["error"] = "not accepting players";
        sendJson(clientFd, err);
        return false;
    }
    // check poprawności kodu pokoju i nazwy
    string room = msg.value("room", "");
    string name = msg.value("name", "");
    if (room != game.roomCode || name.empty()) {
        json err;
        err["error"] = "invalid join";
        sendJson(clientFd, err);
        return false;
    }
    // dodaj gracza + wyznacz hosta jeśli pierwszy
    Player p;
    p.id = game.nextPlayerId++;
    p.name = name;
    p.totalScore = 0;
    p.isHost = game.players.empty();
    p.answered = false;
    p.socketFd = clientFd;
    game.players[p.id] = p;
    // odpowiedz join_ok i rozgłoś lobby
    json ok;
    ok["type"] = "join_ok";
    ok["id"] = p.id;
    if (p.isHost) ok["host"] = true;
    sendJson(clientFd, ok);
    broadcast(game, jsonLobby(game));
    return p.isHost;
}

// obsluga answer (zapisuje odpowiedz gracza w czasie pytania)
void handleAnswer(Game &game, int clientFd, const json &msg) {
    // check poprawnego stanu gry
    if (game.state != GameState::QUESTION_ACTIVE) return;
    int questionId = msg.value("question_id", -1);
    int answerIndex = msg.value("answer", -1);

    // check czy odpowiedź ma potrzebne dane
    if (questionId < 0 || answerIndex < 0) return;

    // check czy serwer ma aktualne pytanie
    if (game.currentQuestionIndex < 0 || game.currentQuestionIndex >= (int)game.questions.size()) return;

    const Question &q = game.questions[game.currentQuestionIndex];
    if (q.id != questionId) return;

    // check czy miescimy się w czasie pytania
    long long t = nowMs();
    long long delta = t - game.questionStartTime;
    if (delta > q.timeLimitMs) return;

    // check czy znamy gracza po jego gniezdzie
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;

    // check czy gracz nie odpowiedzial drugi raz i nie jest hostem
    auto &player = game.players[playerId];
    if (player.isHost) return;
    if (player.answered) return;

    // dodajemy odpowiedź do struktury gry
    player.answered = true;
    Answer a;
    a.playerId = playerId;
    a.answerIndex = answerIndex;
    a.receiveTimeMs = t;
    game.answers.push_back(a);
}

// obsluga begin_quiz (start pytania lub przejscie dalej)
void handleStart(Game &game, int clientFd) {
    // check czy znamy gracza
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;

    // check czy to host bo tylko host może zacząć
    const auto &player = game.players[playerId];
    if (!player.isHost) return;

    // check czy stan pozwala rozpocząć sekwencję pytań
    if (game.state != GameState::LOBBY && game.state != GameState::QUESTION_RESULTS) return;

    // przy pierwszym starcie resetujemy indeks
    if (game.state == GameState::LOBBY) {
        game.currentQuestionIndex = -1;
    }
    startQuestion(game);
}

// obsluga reset_game (tylko host i tylko po FINISHED)
void handleReset(Game &game, int clientFd) {
    // check czy znamy gracza
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;
    // check czy to host
    if (!game.players[playerId].isHost) return;
    // reset możliwy tylko po zakończeniu gry
    if (game.state != GameState::FINISHED) return;
    resetGame(game);
}

// obsluga next_question (host przechodzi do kolejnego pytania lub konczy gre)
void handleNext(Game &game, int clientFd) {
    // check czy znamy gracza
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;
    // check czy to host
    if (!game.players[playerId].isHost) return;
    // check czy jestesmy w stanie z wynikami pytania
    if (game.state != GameState::QUESTION_RESULTS) return;
    // zdecyduj: koniec gry lub kolejne pytanie
    if (game.currentQuestionIndex + 1 >= (int)game.questions.size()) {
        finishGame(game);
    } else {
        startQuestion(game);
    }
}

// obsluga create_game (host tworzy nowa gre i dostaje room code)
void handleCreateGame(Game &game, int clientFd, const json &msg) {
    // check czy nie trwa inna gra
    if (game.state != GameState::NO_GAME && game.state != GameState::FINISHED) {
        json err;
        err["error"] = "game already exists";
        sendJson(clientFd, err);
        return;
    }
    // reset stanu
    resetAll(game);
    game.roomCode = generateRoomCode();
    game.state = GameState::SETUP;

    // utwórz hosta
    string name = msg.value("name", "");
    if (name.empty()) name = "host";
    Player p;
    p.id = game.nextPlayerId++;
    p.name = name;
    p.totalScore = 0;
    p.isHost = true;
    p.answered = false;
    p.socketFd = clientFd;
    game.players[p.id] = p;

    json res;
    res["type"] = "create_ok";
    res["room"] = game.roomCode;
    res["id"] = p.id;
    res["host"] = true;
    sendJson(clientFd, res);
}

// obsluga add_question (host dodaje pytanie w stanie SETUP)
void handleAddQuestion(Game &game, int clientFd, const json &msg) {
    if (game.state != GameState::SETUP) return;
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;
    if (!game.players[playerId].isHost) return;

    Question q;
    q.id = (int)game.questions.size() + 1;
    q.text = msg.value("text", "");
    q.answers = msg.value("answers", vector<string>{});
    q.correct = msg.value("correct", -1);
    q.timeLimitMs = msg.value("time_limit_ms", 10000);

    if (q.text.empty() || q.answers.empty() || q.correct < 0 || q.correct >= (int)q.answers.size()) {
        json err;
        err["error"] = "invalid question";
        sendJson(clientFd, err);
        return;
    }

    game.questions.push_back(q);
    json ok;
    ok["type"] = "add_question_ok";
    ok["question_id"] = q.id;
    sendJson(clientFd, ok);
}

// obsluga start_game (host otwiera lobby dla graczy w stanie SETUP)
void handleOpenLobby(Game &game, int clientFd) {
    int playerId = findPlayerIdByFd(game, clientFd);
    if (playerId == -1) return;
    if (!game.players[playerId].isHost) return;
    if (game.state != GameState::SETUP) return;
    if (game.questions.empty()) {
        json err;
        err["error"] = "no questions";
        sendJson(clientFd, err);
        return;
    }
    game.state = GameState::LOBBY;
    broadcast(game, jsonLobby(game));
    // poinformuj hosta o gotowości
    json ok;
    ok["type"] = "lobby_open";
    ok["room"] = game.roomCode;
    sendJson(clientFd, ok);
}

// sprawdza czy pytanie sie nie skonczylo czasowo
void checkQuestionTimeout(Game &game) {
    if (game.state != GameState::QUESTION_ACTIVE) return;
    if (game.currentQuestionIndex < 0 || game.currentQuestionIndex >= (int)game.questions.size()) return;
    const Question &q = game.questions[game.currentQuestionIndex];
    long long delta = nowMs() - game.questionStartTime;
    if (delta >= q.timeLimitMs) {
        scoreAnswers(game, q);
        game.state = GameState::QUESTION_RESULTS;
        broadcast(game, jsonQuestionResults(game, q));
    }
}

int main(int argc, char **argv) {
    Game game;

    int port = 4000;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }
    if (port <= 0 || port > 65535) {
        cerr << "invalid port, use 1..65535\n";
        return 1;
    }
    // seed dla rand
    srand((unsigned)nowMs());

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == INVALID_SOCKET) {
        cerr << "failed to create socket\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // zeby mozna szybko restartowac serwer na tym samym porcie
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listenFd, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cerr << "bind failed\n";
        close(listenFd);
        return 1;
    }

    if (listen(listenFd, 10) == SOCKET_ERROR) {
        cerr << "listen failed\n";
        close(listenFd);
        return 1;
    }

    // bufory do skladania wiadomosci od klientow
    map<int, string> recvBuffers;

    cout << "quiz server on port " << port << " waiting for create_game\n";

    // głównaa petla serwera
    while (true) {
        // select: czekamy na dane od ktorego kolwiek klienta + accept nowych
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenFd, &readfds);
        int maxfd = listenFd;
        for (const auto &entry : recvBuffers) {
            int fd = entry.first;
            FD_SET(fd, &readfds);
            if (fd > maxfd) maxfd = fd;
        }
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 0.5s tick

        int activity = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
        if (activity < 0) {
            cerr << "select error\n";
            break;
        }

        // nowy klient chce sie polaczyc
        if (FD_ISSET(listenFd, &readfds)) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int clientFd = accept(listenFd, (sockaddr *)&caddr, &clen);
            if (clientFd != INVALID_SOCKET) {
                // zaczynamy sledzic tego klienta (nawet zanim dolaczy do gry)
                recvBuffers[clientFd] = "";
                cout << "new client connected to server\n";
            }
        }

        vector<int> disconnected;
        for (const auto &entry : recvBuffers) {
            int fd = entry.first;
            if (FD_ISSET(fd, &readfds)) {
                char buf[1024];
                int bytes = recv(fd, buf, sizeof(buf) - 1, 0);
                if (bytes <= 0) { // klient sie rozlaczyl, wrzucamy go na liste do usuniecia
                    disconnected.push_back(fd);
                } else {
                    buf[bytes] = 0;
                    recvBuffers[fd] += string(buf);
                    size_t pos;
                    while ((pos = recvBuffers[fd].find('\n')) != string::npos) {
                        // ramkujemy wiadomosci po znaku \n
                        string line = recvBuffers[fd].substr(0, pos);
                        recvBuffers[fd].erase(0, pos + 1);
                        json msg = json::parse(line, nullptr, false);
                        if (msg.is_discarded()) {
                            json err;
                            err["error"] = "bad json";
                            sendJson(fd, err);
                            continue;
                        }

                        // dispatcher po polu "type"
                        string type = msg.value("type", "");
                        if (type == "join") {
                            handleJoin(game, fd, msg);
                        } else if (type == "create_game") {
                            handleCreateGame(game, fd, msg);
                        } else if (type == "add_question") {
                            handleAddQuestion(game, fd, msg);
                        } else if (type == "start_game") {
                            handleOpenLobby(game, fd);
                        } else if (type == "begin_quiz") {
                            handleStart(game, fd);
                        } else if (type == "answer") {
                            handleAnswer(game, fd, msg);
                        } else if (type == "next_question") {
                            handleNext(game, fd);
                        } else if (type == "reset_game") {
                            handleReset(game, fd);
                        } else {
                            json err;
                            err["error"] = "unknown type";
                            sendJson(fd, err);
                        }
                    }
                }
            }
        }

        // zamknij rozlaczone sockety
        for (int fd : disconnected) {
            cout << "client disconnected\n";
            recvBuffers.erase(fd);
            if (findPlayerIdByFd(game, fd) != -1) {
                closeClient(game, fd);
            } else {
                close(fd);
            }
        }

        // sprawdz timeout pytania (dziala nawet jak nikt nic nie wysyla)
        checkQuestionTimeout(game);
    }

    close(listenFd);
    return 0;
}
