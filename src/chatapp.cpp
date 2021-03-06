#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// util
#include "dice_roll.hpp"
#include "pseudo_random.hpp"
#include "stats.hpp"

// caf
#include "caf/all.hpp"
#include "caf/io/middleman.hpp"

/// clients actions
enum action { post, leave, invite, compute, none };

// types
using client_map = std::unordered_map<std::uint64_t, caf::actor>;
using friend_set = std::unordered_set<caf::actor>;
using chat_set = std::unordered_set<caf::actor>;
using client_set = std::unordered_set<caf::actor>;
using action_map = std::unordered_map<action, std::uint64_t>;

// messages
using post_atom = caf::atom_constant<caf::atom("post")>;
using forward_atom = caf::atom_constant<caf::atom("forward")>;
using bump_atom = caf::atom_constant<caf::atom("bump")>;
using stop_atom = caf::atom_constant<caf::atom("stop")>;
using join_atom = caf::atom_constant<caf::atom("join")>;
using leave_atom = caf::atom_constant<caf::atom("leave")>;
using left_atom = caf::atom_constant<caf::atom("left")>;
using befriend_atom = caf::atom_constant<caf::atom("befriend")>;
using logout_atom = caf::atom_constant<caf::atom("logout")>;
using invite_atom = caf::atom_constant<caf::atom("invite")>;
using act_atom = caf::atom_constant<caf::atom("act")>;
using login_atom = caf::atom_constant<caf::atom("login")>;
using finished_atom = caf::atom_constant<caf::atom("finished")>;
using poke_atom = caf::atom_constant<caf::atom("poke")>;
using disconnect_atom = caf::atom_constant<caf::atom("disconnect")>;
using confirm_atom = caf::atom_constant<caf::atom("confirm")>;
using print_atom = caf::atom_constant<caf::atom("print")>;
using collect_atom = caf::atom_constant<caf::atom("collect")>;
using apply_atom = caf::atom_constant<caf::atom("apply")>;
using complete_atom = caf::atom_constant<caf::atom("complete")>;
using append_atom = caf::atom_constant<caf::atom("append")>;
using quit_atom = caf::atom_constant<caf::atom("quit")>;

/// simulates extern client events for each turn
struct behavior_factory {
  behavior_factory() = default;

  behavior_factory(std::uint64_t compute, std::uint64_t post,
                   std::uint64_t leave, std::uint64_t invite)
    : compute(compute), post(post), leave(leave), invite(invite) {
    // nop
  }

  behavior_factory(const behavior_factory& f) = default;

  action apply(dice_roll dice) const {
    auto next_action = action::none;

    if (dice.apply(compute)) {
      next_action = action::compute;
    } else if (dice.apply(post)) {
      next_action = action::post;
    } else if (dice.apply(leave)) {
      next_action = action::leave;
    } else if (dice.apply(invite)) {
      next_action = action::invite;
    }

    return next_action;
  }

  std::uint64_t compute;
  std::uint64_t post;
  std::uint64_t leave;
  std::uint64_t invite;
};

template <class Inspector>
typename Inspector::result_type inspect(Inspector& f, behavior_factory& x) {
  return f(caf::meta::type_name("behavior_factory"), x.compute, x.post, x.leave,
           x.invite);
}

/// for clients compute turn
std::uint64_t fibonacci(std::uint8_t x) {
  if (x == 0 || x == 1) {
    return x;
  } else {
    auto j = x / 2;
    auto fib_j = fibonacci(j);
    auto fib_i = fibonacci(j - 1);

    if (x % 2 == 0) {
      return fib_j * (fib_j + (fib_i * 2));
    } else if (x % 4 == 1) {
      return (((fib_j * 2) + fib_i) * ((fib_j * 2) - fib_i)) + 2;
    } else {
      return (((fib_j * 2) + fib_i) * ((fib_j * 2) - fib_i)) - 2;
    }
  }
}

struct chat_state {
  chat_state() : name("chat") {
    // nop
  }
  client_set members;
  std::vector<std::vector<std::uint8_t>> buffer;
  const char* name;
};

caf::behavior chat(caf::stateful_actor<chat_state>* self,
                   const caf::actor initiator) {
  self->state.members.insert(initiator);
  return {
    [=](post_atom, std::vector<std::uint8_t>& payload,
        const caf::actor& accumulator) {
      auto& s = self->state;
#ifndef BENCH_NO_BUFFERED_CHATS
      s.buffer.push_back(payload);
#endif
      if (s.members.empty()) {
        self->send(accumulator, stop_atom::value, action::post);
      } else {
        self->send(accumulator, bump_atom::value, s.members.size());
        for (auto& member : s.members) {
          self->send(member, forward_atom::value, self, payload, accumulator);
        }
      }
    },
    [=](join_atom, const caf::actor& client, const caf::actor& accumulator) {
      auto& s = self->state;
      s.members.insert(client);
#ifdef BENCH_NO_BUFFERED_CHATS
      self->send(accumulator, stop_atom::value, action::invite);
#else
      if (s.buffer.empty()) {
        self->send(accumulator, stop_atom::value, action::invite);
      } else {
        self->send(accumulator, bump_atom::value, s.buffer.size());
        for (auto& message : s.buffer) {
          self->send(client, forward_atom::value, self, message, accumulator);
        }
      }
#endif
    },
    [=](leave_atom, const caf::actor& client, const bool did_logout,
        const caf::actor& accumulator) {
      self->state.members.erase(client);
      self->send(client, left_atom::value, self, did_logout, accumulator);
      if (self->state.members.empty()) {
        self->quit();
      }
    },
    [=](leave_atom, const caf::actor& client, const bool did_logout) {
      self->state.members.erase(client);
      self->send(client, left_atom::value, self, did_logout);
      if (self->state.members.empty()) {
        self->quit();
      }
    },
  };
}

struct client_state {
  client_state() : name("client") {
    // nop
  }
  std::uint64_t id{};
  friend_set friends;
  chat_set chats;
  caf::actor directory;
  dice_roll dice;
  pseudo_random rand;
  const char* name;
};

caf::behavior client(caf::stateful_actor<client_state>* self,
                     const std::uint64_t id, const caf::actor directory,
                     std::uint64_t seed) {
  auto& s = self->state;
  s.id = id;
  s.directory = directory;
  s.rand = pseudo_random(seed);
  s.dice = dice_roll(s.rand);

  return {
    [=](befriend_atom, const caf::actor& client) {
      self->state.friends.insert(client);
    },
    [=](logout_atom) {
      auto& s = self->state;
      if (s.chats.empty()) {
        self->send(s.directory, left_atom::value, s.id);
      } else {
        for (auto& chat : s.chats) {
          self->send(chat, leave_atom::value, self, true);
        }
      }
    },
    [=](left_atom, const caf::actor& chat, const bool did_logout,
        const caf::actor& accumulator) {
      auto& s = self->state;
      s.chats.erase(chat);
      self->send(accumulator, stop_atom::value, action::leave);
    },
    [=](left_atom, const caf::actor& chat, const bool did_logout) {
      auto& s = self->state;
      s.chats.erase(chat);
      if (did_logout && s.chats.empty()) {
        self->send(s.directory, left_atom::value, s.id);
        self->quit(); // TODO aks pony about this
      }
    },
    [=](invite_atom, const caf::actor& chat, const caf::actor& accumulator) {
      self->state.chats.insert(chat);
      self->send(chat, join_atom::value, self, accumulator);
    },
    [=](forward_atom, const caf::actor& chat,
        std::vector<std::uint8_t>& payload, const caf::actor& accumulator) {
      self->send(accumulator, stop_atom::value, action::post);
    },
    [=](act_atom, behavior_factory& behavior, const caf::actor& accumulator) {
      auto& s = self->state;
      caf::actor next_chat;
      if (s.chats.empty()) {
        next_chat = self->spawn(chat, self);
      } else {
        size_t index = s.rand.next_int(s.chats.size());
        next_chat = *std::next(s.chats.begin(), index);
      }

      switch (behavior.apply(s.dice)) {
        case action::post:
          self->send(next_chat, post_atom::value, std::vector<std::uint8_t>{},
                     accumulator);
          break;
        case action::leave:
          self->send(next_chat, leave_atom::value, self, false, accumulator);
          break;
        case action::compute:
          fibonacci(35);
          self->send(accumulator, stop_atom::value, action::compute);
          break;
        case action::none:
          self->send(accumulator, stop_atom::value, action::none);
          break;
        case action::invite:
          std::vector<caf::actor> f;
          for (auto& i : s.friends) {
            f.push_back(i);
          }
          auto rng = std::default_random_engine{s.rand.next_int()};
          std::shuffle(f.begin(), f.end(), rng);
          f.insert(f.begin(), self);

          std::size_t invitations = s.rand.next_int(s.friends.size());
          if (invitations == 0) {
            self->send(accumulator, stop_atom::value, action::invite);
          } else {
            self->send(accumulator, bump_atom::value, invitations);
            for (size_t i = 0; i < invitations; ++i) {
              self->send(f[i], invite_atom::value, next_chat, accumulator);
            }
          }
          break;
      }
    },
  };
}

struct directory_state {
  directory_state() : name("directory") {
    // nop
  }
  client_map clients;
  pseudo_random random;
  std::uint32_t befriend{};
  bool is_poker = false;
  caf::actor poker;
  const char* name;
};

caf::behavior directory(caf::stateful_actor<directory_state>* self,
                        std::uint64_t seed, std::uint32_t befriend) {
  auto& s = self->state;
  s.random = pseudo_random(seed);
  s.befriend = befriend;
  return {
    [=](login_atom, std::uint64_t id) {
      auto& s = self->state;
      auto new_client = self->spawn(client, id, self, s.random.next_int());
      s.clients.emplace(id, new_client);
      for (auto& client : s.clients) {
        if (s.random.next_int(100) < s.befriend) {
          self->send(client.second, befriend_atom::value, new_client);
          self->send(new_client, befriend_atom::value, client.second);
        }
      }
    },
    [=](logout_atom, std::uint64_t id) {
      self->send(self->state.clients.at(id), logout_atom::value);
    },
    [=](left_atom, std::uint64_t id) {
      auto& s = self->state;
      s.clients.erase(id);
      if (s.clients.empty() && s.is_poker) {
        self->send(s.poker, finished_atom::value);
      }
    },
    [=](poke_atom, behavior_factory& behavior, const caf::actor& accumulator) {
      for (auto& client : self->state.clients) {
        self->send(client.second, act_atom::value, behavior, accumulator);
      }
    },
    [=](disconnect_atom, caf::actor& poker) {
      auto& s = self->state;
      s.is_poker = true;
      s.poker = poker;

      for (auto& client : s.clients) {
        self->send(client.second, logout_atom::value);
      }
    },
    [=](quit_atom) { self->quit(); },
  };
}

using time_point = std::chrono::time_point<std::chrono::high_resolution_clock>;
struct accumulator_state {
  accumulator_state() : name("accumulator") {
    // nop
  }
  caf::actor poker;
  action_map actions;
  time_point start;
  time_point end;
  /// time in milliseconds
  double duration{};
  size_t expected{};
  bool did_stop{};
  const char* name;
};

caf::behavior accumulator(caf::stateful_actor<accumulator_state>* self,
                          caf::actor poker, std::size_t expected) {
  auto& s = self->state;
  s.poker = poker;
  s.start = std::chrono::high_resolution_clock::now();
  s.expected = expected;
  s.did_stop = false;
  s.actions.emplace(action::invite, 0);
  s.actions.emplace(action::none, 0);
  s.actions.emplace(action::compute, 0);
  s.actions.emplace(action::post, 0);
  s.actions.emplace(action::leave, 0);
  return {
    [=](bump_atom, const std::size_t expected) {
      auto& s = self->state;
      s.expected = (s.expected + expected) - 1;
    },
    [=](stop_atom, const action act) {
      auto& s = self->state;
      s.actions.at(act)++;
      --s.expected;
      if (s.expected == 1) {
        s.end = std::chrono::high_resolution_clock::now();
        s.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                       s.end - s.start)
                       .count();
        s.did_stop = true;

        self->send(s.poker, confirm_atom::value);
      }
    },
    [=](print_atom, const caf::actor& poker, std::size_t i, std::size_t j) {
      self->send(poker, collect_atom::value, i, j, self->state.duration,
                 self->state.actions);
      self->quit();
    },
  };
}

struct poker_state {
  poker_state() : name("poker") {
    // nop
  }
  action_map actions;
  std::uint64_t clients{};
  std::size_t logouts{};
  std::size_t confirmations{};
  std::uint64_t turns{};
  std::uint64_t runs{};
  std::size_t iteration{};
  std::vector<caf::actor> directories;
  std::vector<caf::actor> runtimes;
  std::size_t accumulations{};
  std::vector<std::vector<double>> finals;
  behavior_factory factory{};
  caf::actor bench;
  bool last{};
  std::vector<double> turn_series;
  const char* name;
};

caf::behavior poker(caf::stateful_actor<poker_state>* self,
                    std::uint64_t clients, std::uint64_t turns,
                    std::uint64_t runs, std::vector<caf::actor> directories,
                    behavior_factory factory) {
  auto& s = self->state;
  s.clients = clients;
  s.logouts = 0;
  s.confirmations = 0;
  s.turns = turns;
  s.iteration = 0;
  s.runs = runs;
  s.directories = std::move(directories);
  s.factory = factory;
  s.finals.resize(s.runs, std::vector<double>(s.turns));
  s.actions.emplace(action::invite, 0);
  s.actions.emplace(action::none, 0);
  s.actions.emplace(action::compute, 0);
  s.actions.emplace(action::post, 0);
  s.actions.emplace(action::leave, 0);
  return {
    [=](apply_atom, caf::actor& bench, bool last) {
      auto& s = self->state;
      s.confirmations = s.turns;
      s.logouts = s.directories.size();
      s.bench = bench;
      s.last = last;
      s.accumulations = 0;

      std::uint64_t turns = s.turns;
      std::size_t index = 0;

      for (size_t client = 0; client < s.clients; ++client) {
        index = client % s.directories.size();
        self->send(s.directories.at(index), login_atom::value, client);
      }
      // feetback loop?
      for (; turns > 0; --turns) {
        auto accu = self->spawn(accumulator, self, s.clients);
        for (auto& directory : s.directories) {
          self->send(directory, poke_atom::value, s.factory, accu);
        }

        s.runtimes.push_back(accu);
      }
    },
    [=](confirm_atom) {
      auto& s = self->state;
      --s.confirmations;
      if (s.confirmations == 1) {
        for (auto& d : s.directories) {
          self->send(d, disconnect_atom::value, self);
        }
      }
    },
    [=](finished_atom) {
      auto& s = self->state;
      --s.logouts;
      if (s.logouts == 1) {
        std::size_t turn = 0;

        for (auto& accumulator : s.runtimes) {
          ++s.accumulations;
          self->send(accumulator, print_atom::value, self, s.iteration, turn);
          ++turn;
        }

        s.runtimes.clear();
      }
    },
    [=](collect_atom, std::size_t i, std::size_t j, double duration,
        const action_map& actions) {
      auto& s = self->state;
      for (auto& act : actions) {
        s.actions.at(act.first) += act.second;
      }
      try {
        s.finals.at(i).at(j) = duration;
      } catch (std::exception& e) {
        caf::aout(self) << "exception at " << i << " " << j << std::endl;
      }

      s.turn_series.push_back(duration);

      --s.accumulations;
      if (s.accumulations == 0) {
        caf::aout(self) << "iteration end" << std::endl;
        ++s.iteration;
        self->send(s.bench, complete_atom::value);

        if (s.last) {
          caf::aout(self) << "start data collection" << std::endl;
          sample_stats stats(s.turn_series);
          std::vector<std::vector<double>> turns;
          std::vector<double> qos;

          // TODO ask pony about line 381 to 391

          for (std::size_t l = 0; l < s.finals.size(); ++l) {
            qos.push_back(sample_stats(s.finals.back()).stddev());
            s.finals.pop_back();
          }

          std::stringstream title_text;
          title_text << std::string(31, ' ') << std::setw(18) << "j-mean"
                     << std::setw(18) << "j-median" << std::setw(18)
                     << "j-error" << std::setw(18) << "j-stddev"
                     << std::setw(32) << "quality of service" << std::endl;

          std::stringstream result_text;
          result_text << "Turns" << std::string(27, ' ') << std::setw(17)
                      << stats.mean() << " " << std::setw(17) << stats.median()
                      << " " << std::setw(17) << stats.err() << " "
                      << std::setw(17) << stats.stddev() << " " << std::setw(31)
                      << sample_stats(qos).median() << std::endl;

          std::stringstream act_text;
          act_text << std::endl
                   << "Acts:" << std::endl
                   << "Post: " << s.actions.at(action::post) << std::endl
                   << "Leave: " << s.actions.at(action::leave) << std::endl
                   << "Invite: " << s.actions.at(action::invite) << std::endl
                   << "Compute: " << s.actions.at(action::compute) << std::endl
                   << "None: " << s.actions.at(action::none) << std::endl;

          self->send(s.bench, append_atom::value, title_text.str(),
                     result_text.str(), act_text.str());
          for (auto& d : s.directories) {
            self->send(d, quit_atom::value);
          }
          self->quit();
        }
      }
    },
  };
}

struct config : caf::actor_system_config {
  std::uint64_t run = 10;
  std::size_t directories = 8;
  std::uint64_t clients = 1024;
  std::uint64_t turns = 32;
  std::uint64_t compute = 75;
  std::uint64_t post = 25;
  std::uint64_t leave = 25;
  std::uint64_t invite = 25;
  std::uint64_t befriend = 10;
  config() {
    add_message_type<std::vector<std::uint8_t>>("std::vector<uint8_t>");
    add_message_type<std::vector<double>>("std::vector<double>");
    add_message_type<std::size_t>("size_t");
    add_message_type<std::uint64_t>("uint64_t");
    add_message_type<behavior_factory>("behavior_factory");
    opt_group{custom_options_, "global"}
      .add(run, "run,r", "The number of iterations. Defaults to 10")
      .add(clients, "clients,c", "The number of directories. Defaults to 1024.")
      .add(directories, "dir,d", "The number of directories. Defaults to 8.")
      .add(turns, "turns,t", "The number of turns. Defaults to 32.")
      .add(compute, "compute,m",
           "The compute behavior probability. Defaults to 75.")
      .add(post, "post,p", "The post behavior probability. Defaults to 25.")
      .add(leave, "leave,l", "The leave behavior probability. Defaults to 25.")
      .add(invite, "invite,d",
           "The invite behavior probability. Defaults to 25.")
      .add(befriend, "befriend,b", "The befriend probability. Defaults to 10.");
  }
};

struct chatapp_state {
  chatapp_state() : name("chatapp") {
    // nop
  }
  std::uint64_t clients{};
  std::uint64_t turns{};
  std::uint64_t run{};
  std::vector<caf::actor> directories;
  behavior_factory factory{};
  caf::actor poker;
  const char* name;
};

caf::behavior chatapp(caf::stateful_actor<chatapp_state>* self,
                      const std::uint64_t clients, const std::uint64_t turns,
                      const std::uint64_t run, const std::size_t directories,
                      const std::uint64_t compute, const std::uint64_t post,
                      const std::uint64_t leave, const std::uint64_t invite,
                      const std::uint64_t befriend) {
  auto& s = self->state;
  s.clients = clients;
  s.turns = turns;
  s.run = run;
  pseudo_random rand(42);

  s.factory = behavior_factory(compute, post, leave, invite);

  for (std::size_t i = 0; i < directories; ++i) {
    s.directories.emplace_back(
      self->spawn(directory, rand.next_int(), befriend));
  }

  s.poker = self->spawn(poker, s.clients, s.turns, s.run, s.directories,
                        s.factory);

  return {
    [=](apply_atom, caf::actor& async_benchmark_completion, bool last) {
      self->send(self->state.poker, apply_atom::value,
                 async_benchmark_completion, last);
    },
  };
}

void caf_main(caf::actor_system& system, const config& cfg) {
  auto chat = system.spawn(chatapp, cfg.clients, cfg.turns, cfg.run,
                           cfg.directories, cfg.compute, cfg.post, cfg.leave,
                           cfg.invite, cfg.befriend);
  caf::scoped_actor self{system};
  std::vector<double> durations;
  std::stringstream title_text;
  title_text << std::string(31, ' ') << std::setw(18) << "i-mean"
             << std::setw(18) << "i-median" << std::setw(18) << "i-error"
             << std::setw(18) << "i-stddev" << std::endl;
  std::cout << title_text.str();
  for (std::uint64_t i = 1; i < cfg.run; ++i) {
    caf::aout(self) << "start" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    self->send(chat, apply_atom::value, self, false);
    self->receive([&](complete_atom) {
      auto end = std::chrono::high_resolution_clock::now();
      durations.emplace_back(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count());
      sample_stats stats(durations);
    });
  }
  caf::aout(self) << "start" << std::endl;
  auto start = std::chrono::high_resolution_clock::now();
  self->send(chat, apply_atom::value, self, true);
  self->receive([&](complete_atom) {
    auto end = std::chrono::high_resolution_clock::now();
    durations.emplace_back(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
        .count());
  });
  self->receive([&](append_atom, std::string& title, std::string& result,
                    std::string& act) {
    sample_stats stats(durations);
    std::stringstream result_text;
    result_text << "ChatApp" << std::string(25, ' ') << std::setw(17)
                << stats.mean() << " " << std::setw(17) << stats.median() << " "
                << std::setw(17) << stats.err() << " " << std::setw(17)
                << stats.stddev() << " " << std::setw(17) << std::endl;
    std::cout << result_text.str();
    std::cout << title << result << act;
  });
}

CAF_MAIN(caf::io::middleman)
