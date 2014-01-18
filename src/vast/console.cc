#include "vast/console.h"

#include <cassert>
#include <iomanip>
#include <cppa/cppa.hpp>
#include "vast/event.h"
#include "vast/util/parse.h"

using namespace cppa;

namespace vast {

namespace {

struct cow_event_less_than
{
  bool operator()(cow<event> const& x, cow<event> const& y) const
  {
    return *x < *y;
  }
};

cow_event_less_than cow_event_lt;

} // namespace <anonymous>

console::console(cppa::actor_ptr search, path dir)
  : dir_{std::move(dir)},
    search_{std::move(search)}
{
  if (! exists(dir_))
    if (mkdir(dir_))
      VAST_LOG_ACTOR_ERROR("failed to create console directory: " << dir_);

  cmdline_.mode_add("main",
                    "main mode",
                    "::: ",
                    to<std::string>(dir_ / "history_main"));

  cmdline_.mode_push("main");
  cmdline_.on_unknown_command(
      "main",
      [=](std::string arg)
      {
        if (! arg.empty())
          std::cerr
            << "[error] invalid command: " << arg
            << ", try 'help'" << std::endl;
        return true;
      });

  cmdline_.cmd_add(
      "main",
      "exit",
      [=](std::string)
      {
        quit(exit::stop);
        return false;
      });

  cmdline_.cmd_add(
      "main",
      "help",
      [=](std::string)
      {
        auto help =
          "ask         enter query mode (leave with 'exit')\n"
          "list        show all queries (active prefixed with *)\n"
          "query <id>  enter control mode of given query\n"
          "set <..>    settings\n"
          "exit        exit the console";
        std::cerr << help << std::endl;
        return true;
      });

  // TODO: consider using a settings mode instead of just a single command.
  cmdline_.cmd_add(
      "main",
      "set",
      [=](std::string arg)
      {
        auto help = [=]()
        {
          auto help =
            "paginate <n>       number of results to display\n"
            "auto-follow <T|F>  follow query after creation\n"
            "show               shows current settings";

          std::cerr << help << std::endl;
        };

        auto show = [=]
        {
          std::cerr
            << "paginate = " << opts_.paginate << '\n'
            << "auto-follow = " << (opts_.auto_follow ? "T" : "F")
            << std::endl;
        };

        if (arg.empty())
        {
          show();
          return true;
        }

        match_split(arg, ' ')(
            on("paginate", arg_match) >> [=](std::string const& str)
            {
              uint64_t n;
              auto begin = str.begin();
              if (extract(begin, str.end(), n))
                opts_.paginate = n;
              else
                std::cerr
                  << "[error] paginate requires numeric argument" << std::endl;
            },
            on("auto-follow", "T") >> [=](std::string const&)
            {
              opts_.auto_follow = true;
            },
            on("auto-follow", "F") >> [=](std::string const&)
            {
              opts_.auto_follow = false;
            },
            on("show") >> show,
            on("help") >> help,
            on("?") >> help,
            others() >> [=]
            {
              std::cerr << "[error] invalid argument '" << arg << "'";
              help();
            });

        return true;
      });

  cmdline_.cmd_add(
      "main",
      "ask",
      [=](std::string)
      {
        cmdline_.mode_push("ask");
        return true;
      });


  cmdline_.cmd_add(
      "main",
      "list",
      [=](std::string)
      {
        for (auto& p : results_)
          std::cout
            << (&p.second == current_result_ ? " * " : "   ")
            << p.second.id() << '\t'
            << p.second.size()
            << std::endl;
        return true;
      });

  cmdline_.cmd_add(
      "main",
      "query",
      [=](std::string arg)
      {
        if (arg.empty())
        {
          std::cerr
            << "[error] argument required, check 'query help'"
            << std::endl;
          return true;
        }
        auto r = to_result(arg);
        if (r.first)
        {
          VAST_LOG_ACTOR_DEBUG("enters query " << r.second->id());
          current_result_ = r.second;
          current_query_ = r.first;
          send(self, atom("key"), atom("get"));
          return false;
        }
        return true;
      });

  cmdline_.mode_add("ask",
                    "query asking mode",
                    "-=> ",
                    to<std::string>(dir_ / "history_query"));

  cmdline_.cmd_add(
      "ask",
      "exit",
      [=](std::string)
      {
        cmdline_.mode_pop();
        return true;
      });

  cmdline_.on_unknown_command(
      "ask",
      [=](std::string q)
      {
        if (q.empty())
          return true;
        sync_send(search_, atom("query"), atom("create"), q).then(
            on(atom("EXITED"), arg_match) >> [](uint32_t reason)
            {
              std::cerr
                << "[error] search terminated with exit code "
                << reason << std::endl;

              send_exit(self, exit::error);
            },
            on_arg_match >> [=](std::string const& error) // TODO: use error class.
            {
              std::cerr << "[error] syntax error: " << error << std::endl;
              show_prompt();
            },
            on_arg_match >> [=](actor_ptr const& qry, expr::ast const& ast)
            {
              assert(! results_.count(qry));
              assert(qry);
              assert(ast);

              cmdline_.append_to_history(q);
              monitor(qry);
              current_query_ = qry;
              current_result_ = &results_.emplace(qry, ast).first->second;
              cmdline_.mode_pop();
              std::cerr
                << "new query " << current_result_->id()
                << " -> " << ast << std::endl;

              if (opts_.auto_follow)
              {
                follow_mode_ = true;
                send(self, atom("key"), atom("get"));
              }
              else
              {
                show_prompt();
              }
            },
            others() >> [=]
            {
              VAST_LOG_ACTOR_ERROR("got unexpected message: " <<
                                   to_string(last_dequeued()));
              show_prompt();
            });
      return false;
    });
}

console::result::result(expr::ast ast)
  : ast_{std::move(ast)}
{
}

void console::result::add(cow<event> e)
{
  auto i = std::lower_bound(events_.begin(), events_.end(), e, cow_event_lt);
  assert(i == events_.end() || cow_event_lt(e, *i));
  events_.insert(i, e);
}

size_t console::result::apply(size_t n, std::function<void(event const&)> f)
{
  size_t i = 0;
  while (i < n && pos_ < events_.size())
  {
    f(*events_[pos_++]);
    ++i;
  }
  return i;
}

size_t console::result::seek_forward(size_t n)
{
  if (pos_ + n >= events_.size())
  {
    auto seeking = static_cast<size_t>(events_.size() - pos_);
    pos_ = events_.size();
    return seeking;
  }
  else
  {
    pos_ += n;
    return n;
  }
}

size_t console::result::seek_backward(size_t n)
{
  if (n > pos_)
  {
    auto old = pos_;
    pos_ = 0;
    return static_cast<size_t>(old);
  }
  else
  {
    pos_ -= n;
    return n;
  }
}

expr::ast const& console::result::ast() const
{
  return ast_;
}

size_t console::result::size() const
{
  return events_.size();
}

void console::act()
{
  chaining(false);

  auto keystroke_monitor = spawn<detached+linked>(
      [=]
      {
        become(
            on(atom("get")) >> [=]
            {
              char c;
              return make_any_tuple(atom("key"), cmdline_.get(c) ? c : 'q');
            });
      });

  become(
      on(atom("DOWN"), arg_match) >> [=](uint32_t)
      {
        VAST_LOG_ACTOR_ERROR("got DOWN from query @" << last_sender()->id());
        // TODO: seal corresponding result
      },
      on(atom("done")) >> [=]
      {
        VAST_LOG_ACTOR_DEBUG("got done from query @" << last_sender()->id());
        show_prompt();
        // TODO: seal corresponding query.
      },
      on(atom("prompt")) >> [=]
      {
        bool callback_result;
        if (! cmdline_.process(callback_result) || callback_result)
          show_prompt();
      },
      on_arg_match >> [=](event const&)
      {
        auto i = results_.find(last_sender());
        assert(i != results_.end());
        auto r = &i->second;
        cow<event> ce = *tuple_cast<event>(last_dequeued());
        r->add(ce);
        if (r == current_result_ && follow_mode_)
          std::cout << *ce << std::endl;
      },
      on(atom("key"), atom("get")) >> [=]
      {
        send(keystroke_monitor, atom("get"));
      },
      on(atom("key"), arg_match) >> [=](char key)
      {
        switch (key)
        {
          default:
            {
              std::cerr
                << "invalid key: '" << key << "', press '?' for help"
                << std::endl;
            }
            break;
          case '?':
            {
              std::cerr
                << "interactive query control mode:\n\n"
                << "    <space>  display the next batch of available results\n"
                << "       e     ask query for more results\n"
                << "       f     toggle follow mode\n"
                << "       j     seek one batch forward\n"
                << "       k     seek one batch backword\n"
                << "       ?     display this help\n"
                << "       q     leave query control mode\n"
                << std::endl;
            }
            break;
          case ' ':
            {
              auto n = current_result_->apply(
                  opts_.paginate,
                  [](event const& e) { std::cout << e << std::endl; });
              if (n == 0)
                std::cerr
                  << "[query " << current_result_->id() << "] "
                  << "reached end of results" << std::endl;
            }
            break;
          case 'e':
            {
              send(current_query_, atom("extract"));
              std::cerr
                << "[query " << current_result_->id() << "] "
                << "asks for more results" << std::endl;
            }
            break;
          case 'f':
            {
              follow_mode_ = ! follow_mode_;
              std::cerr
                << "[query " << current_result_->id() << "] "
                << "toggled follow-mode to " << (follow_mode_ ? "on" : "off")
                << std::endl;
            }
            break;
          case 'j':
            {
              auto n = current_result_->seek_forward(opts_.paginate);
              std::cerr
                << "[query " << current_result_->id() << "] "
                << "seeked +" << n << " events" << std::endl;
            }
            break;
          case 'k':
            {
              auto n = current_result_->seek_backward(opts_.paginate);
              std::cerr
                << "[query " << current_result_->id() << "] "
                << "seeked -" << n << " events" << std::endl;
            }
            break;
          case '':
          case '\n':
          case 'q':
            {
              follow_mode_ = false;
              show_prompt();
            }
            return;
        }
        send(keystroke_monitor, atom("get"));
      },
      others() >> [=]
      {
        VAST_LOG_ACTOR_ERROR("got unexpected message from @" <<
                             last_sender()->id() << ": " <<
                             to_string(last_dequeued()));
      });
}

char const* console::description() const
{
  return "console";
}

void console::show_prompt(size_t ms)
{
  // The delay allows for logging messages to trickle through first
  // before we print the prompt.
  delayed_send(self, std::chrono::milliseconds(ms), atom("prompt"));
}

std::pair<actor_ptr, console::result*>
console::to_result(std::string const& str)
{
  std::vector<result*> matches;
  actor_ptr qry = nullptr;
  for (auto& p : results_)
  {
    auto candidate = to<std::string>(p.second.id());
    auto i = std::mismatch(str.begin(), str.end(), candidate.begin());
    if (i.first == str.end())
    {
      qry = p.first;
      matches.push_back(&p.second);
    }
  }
  if (matches.empty())
    std::cerr << "[error] no such query: " << str << std::endl;
  else if (matches.size() > 1)
    std::cerr << "[error] ambiguous query: " << str << std::endl;
  else
    return {qry, matches[0]};
  return {nullptr, nullptr};
}

} // namespace vast
