/* CENV - C/C++ environments
 *
 * Copyright 2020  Jakub Kaszycki
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <istream>
#include <libgen.h>
#include <list>
#include <ostream>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/types.h>

namespace cenv
{
  class syntax_exception final : public std::logic_error
  {
  public:
    syntax_exception (const char *msg)
      noexcept
      : logic_error {msg}
    {
    }

    syntax_exception (const std::string &msg)
      noexcept
      : logic_error {msg}
    {
    }
  };

  inline void
  substitute_vars (std::istream &input,
                   std::ostream &output,
                   const std::unordered_map<std::string, std::string> &variables)
  {
    struct stack_entry
    {
      bool braced;
      std::string contents;

      stack_entry (bool braced)
        noexcept
        : braced {braced}, contents {}
      {
      }
    };

    bool after_dollar {false};
    std::stack<stack_entry> stack;

    // Helper functions

    const auto is_braced = [&] () -> bool {
      return !stack.empty () && stack.top ().braced;
    };

    const auto is_unbraced = [&] () -> bool {
      return !stack.empty () && !stack.top ().braced;
    };

    const auto lookup = [&] (const std::string &key) -> const std::string & {
      auto itr = variables.find (key);

      if (itr != variables.cend ())
        return itr->second;
      else
        {
          std::ostringstream msg_builder;
          msg_builder << "Unknown variable: " << key;
          throw syntax_exception {msg_builder.str ()};
        }
    };

    const auto push = [&] (bool braced) -> void {
      if (stack.size () >= 1024)
        throw syntax_exception {"Recursion depth limit exceeded in variable"};

      stack.emplace (braced);
    };

    const auto write = [&] (char ch) -> void {
      if (!stack.empty ())
        stack.top ().contents.push_back (ch);
      else
        output.put (ch);
    };

    const auto write_str = [&] (const std::string &str) -> void {
      if (!stack.empty ())
        stack.top ().contents.append (str);
      else
        output.write (str.c_str (), str.size ());
    };

    const auto pop = [&] () -> void {
      assert (!stack.empty ());

      auto str {std::move (stack.top ().contents)};
      stack.pop ();
      write_str (lookup (str));
    };

    // The automaton

    char ch;
    while (input.get (ch))
      switch (ch)
        {
        case '$':
          if (after_dollar)
            {
              after_dollar = false;
              write ('$');
              break;
            }

          if (is_unbraced ())
            pop ();

          after_dollar = true;
          break;

        case '{':
          if (after_dollar)
            {
              after_dollar = false;
              push (true);
              break;
            }

          if (is_unbraced ())
            pop ();

          write ('{');
          break;

        case '}':
          if (is_braced () && !after_dollar)
            {
              pop ();
              break;
            }

          after_dollar = false;
          write ('}');
          break;

        default:
          if (is_unbraced ())
            {
              pop ();
              break;
            }

          if (after_dollar)
            {
              std::ostringstream msg_builder;
              msg_builder << "Invalid variable start character: " << ch;
              throw syntax_exception {msg_builder.str ()};
            }

          [[fallthrough]];

        case '_': case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
        case 'g': case 'h': case 'i': case 'j': case 'k': case 'l': case 'm':
        case 'n': case 'o': case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z': case 'A':
        case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H':
        case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U': case 'V':
        case 'W': case 'X': case 'Y': case 'Z': case '0': case '1': case '2':
        case '3': case '4': case '5': case '6': case '7': case '8': case '9':
          if (after_dollar)
            push (false);

          write (ch);
          break;
        }

    while (!stack.empty ())
      if (is_braced ())
        throw syntax_exception {"Unterminated braced variable"};
      else
        pop ();
  }

  struct substitor
  {
    const std::string &input;
    const std::unordered_map<std::string, std::string> &variables;
  };

  inline std::ostream &
  operator << (std::ostream &stream,
               const substitor &subst)
  {
    std::istringstream input {subst.input};
    substitute_vars (input, stream, subst.variables);
    return stream;
  }

  struct config
  {
    std::unordered_map<std::string, std::string> variables {};

    std::string folder;
    std::string prompt;
    bool prompt_set {false};
    std::string root;
    bool root_set {false};

    std::list<std::string> executable_suffixes {};
    std::list<std::string> include_suffixes {};
    std::list<std::string> info_suffixes {};
    std::list<std::string> library_suffixes {};
    std::list<std::string> manpage_suffixes {};
    std::list<std::string> pkg_config_suffixes {};

    std::unordered_map<std::string, std::string> environment_variables {};

    void
    add_default_configs ()
    {
      if (!prompt_set)
        {
          std::ostringstream prompt_builder;

          prompt_builder << '('
            << (folder.c_str () + folder.find_last_of ('/') + 1)
            << ") ";

          prompt = prompt_builder.str ();
        }

      if (!root_set)
        root = folder;

      executable_suffixes.push_back ("bin");

      include_suffixes.push_back ("include");
      if (variables.find ("mach_type") != variables.cend ())
        include_suffixes.push_back ("include/${mach_type}");

      info_suffixes.push_back ("share/info");

      library_suffixes.push_back ("lib");
      if (variables.find ("mach_type") != variables.cend ())
        library_suffixes.push_back ("lib/${mach_type}");
      // Some x86_64-specific stuff
      if (variables.find ("mach_x32") != variables.cend ())
        library_suffixes.push_back ("libx32");
      if (variables.find ("mach_32") != variables.cend ())
        library_suffixes.push_back ("lib32");
      if (variables.find ("mach_64") != variables.cend ())
        library_suffixes.push_back ("lib64");

      manpage_suffixes.push_back ("man");
      manpage_suffixes.push_back ("share/man");

      pkg_config_suffixes.push_back ("lib/pkgconfig");
      pkg_config_suffixes.push_back ("share/pkgconfig");
      if (variables.find ("mach_type") != variables.cend ())
        pkg_config_suffixes.push_back ("lib/${mach_type}/pkgconfig");
    }

    substitor
    subst (const std::string &str)
      const noexcept
    {
      return { str, variables };
    }

    void
    write_activate_script (std::ostream &output)
      const
    {
      output << "# Activate script generated by cenv\n"
                "# Use the . command in the shell, do not run this script\n"
                "\n"
                "# Args: $1 - variable name\n"
                "__cenv_defined () {\n"
                "  ! [ \"x${!1+x}\" = x ]\n"
                "}\n"
                "# Args: $1 - variable name\n"
                "__cenv_savevar () {\n"
                "  if __cenv_defined \"$1\"; then\n"
                "    printf -v __CENV_$1_DEFINED yes\n"
                "    printf -v __CENV_$1_ORIG \"%s\" \"${!1}\"\n"
                "  fi\n"
                "}\n"
                "# Args: $1 - variable name\n"
                "__cenv_restorevar () {\n"
                "  printf -v __CENV_TMP \"__CENV_%s_DEFINED\" \"$1\"\n"
                "  if [ \"x${!__CENV_TMP}\" = xyes ]; then\n"
                "    printf -v __CENV_TMP \"__CENV_%s_ORIG\" \"$1\"\n"
                "    printf -v $1 \"%s\" \"${!__CENV_TMP}\"\n"
                "    export $1\n"
                "  else\n"
                "    unset $1\n"
                "  fi\n"
                "  unset __CENV_TMP\n"
                "  unset __CENV_$1_DEFINED\n"
                "  unset __CENV_$1_ORIG\n"
                "}\n"
                "deactivate () {\n"
                "  __cenv_restorevar PS1\n";

      if (!executable_suffixes.empty ())
        output << "  __cenv_restorevar PATH\n";

      if (!include_suffixes.empty ())
        output << "  __cenv_restorevar C_INCLUDE_PATH\n";

      if (!info_suffixes.empty ())
        output << "  __cenv_restorevar INFOPATH\n";

      if (!library_suffixes.empty ())
        output << "  __cenv_restorevar LIBRARY_PATH\n"
                  "  __cenv_restorevar LD_LIBRARY_PATH\n"
                  "  __cenv_restorevar DYLD_LIBRARY_PATH\n";

      if (!manpage_suffixes.empty ())
        output << "  __cenv_restorevar MANPATH\n";

      if (!pkg_config_suffixes.empty ())
        output << "  __cenv_restorevar PKG_CONFIG_PATH\n";

      for (const auto &e : environment_variables)
        output << "  __cenv_restorevar " << e.first << "\n";

      output << "}\n";

      output << "__cenv_savevar PS1\n"
                "PS1=\"" << subst (prompt) << "${PS1}\"\n";

      if (!executable_suffixes.empty ())
        {
          output << "__cenv_savevar PATH\n";

          for (const auto &suffix : executable_suffixes)
            output << "PATH=\"" << root << '/' << subst (suffix)
              << "${PATH+:}${PATH}\"\n";

          output << "export PATH\n";
        }

      if (!include_suffixes.empty ())
        {
          output << "__cenv_savevar C_INCLUDE_PATH\n";

          for (const auto &suffix : include_suffixes)
            output << "C_INCLUDE_PATH=\"" << root << '/' << subst (suffix)
              << "${C_INCLUDE_PATH+:}${C_INCLUDE_PATH}\"\n";

          output << "export C_INCLUDE_PATH\n";
        }

      if (!info_suffixes.empty ())
        {
          output << "__cenv_savevar INFOPATH\n";

          for (const auto &suffix : info_suffixes)
            output << "INFOPATH=\"" << root << '/' << subst (suffix)
              << "${INFOPATH+:}${INFOPATH}\"\n";

          output << "export INFOPATH\n";
        }

      if (!library_suffixes.empty ())
        {
          output << "__cenv_savevar LIBRARY_PATH\n";

          for (const auto &suffix : library_suffixes)
            output << "LIBRARY_PATH=\"" << root << '/' << subst (suffix)
              << "${LIBRARY_PATH+:}${LIBRARY_PATH}\"\n";

          output << "export LIBRARY_PATH\n";

          output << "__cenv_savevar LD_LIBRARY_PATH\n";

          for (const auto &suffix : library_suffixes)
            output << "LD_LIBRARY_PATH=\"" << root << '/' << subst (suffix)
              << "${LD_LIBRARY_PATH+:}${LD_LIBRARY_PATH}\"\n";

          output << "export LD_LIBRARY_PATH\n";

          output << "__cenv_savevar DYLD_LIBRARY_PATH\n";

          for (const auto &suffix : library_suffixes)
            output << "DYLD_LIBRARY_PATH=\"" << root << '/' << subst (suffix)
              << "${DYLD_LIBRARY_PATH+:}${DYLD_LIBRARY_PATH}\"\n";

          output << "export DYLD_LIBRARY_PATH\n";
        }

      if (!manpage_suffixes.empty ())
        {
          output << "__cenv_savevar MANPATH\n";

          for (const auto &suffix : manpage_suffixes)
            output << "MANPATH=\"" << root << '/' << subst (suffix)
              << "${MANPATH+:}${MANPATH}\"\n";

          output << "export MANPATH\n";
        }

      if (!pkg_config_suffixes.empty ())
        {
          output << "__cenv_savevar PKG_CONFIG_PATH\n";

          for (const auto &suffix : pkg_config_suffixes)
            output << "PKG_CONFIG_PATH=\"" << root << '/' << subst (suffix)
              << "${PKG_CONFIG_PATH+:}${PKG_CONFIG_PATH}\"\n";

          output << "export PKG_CONFIG_PATH\n";
        }

      for (const auto &e : environment_variables)
        output << "__cenv_savevar " << e.first << "\n"
               << e.first << "=" << subst (e.second) << '\n'
               << "export " << e.first << '\n';
    }
  };
}

inline void
print_usage (std::ostream &stream)
{
  stream << "Usage: cenv [options...] folder\n";
}

inline void
print_help (void)
{
  print_usage (std::cout);

  std::cout << "Options:\n"
               "   -D <KEY>=<VAL> - Add a substition variable\n"
               "   -e <SUFFIX>    - Add an executable suffix\n"
               "   -E <KEY>=<VAL> - Add an extra environment variable\n"
               "   -h             - Print this help text\n"
               "   -i <SUFFIX>    - Add an include suffix\n"
               "   -I <SUFFIX>    - Add an info suffix\n"
               "   -l <SUFFIX>    - Add a library suffix\n"
               "   -m <SUFFIX>    - Add a manpage suffix\n"
               "   -n             - Turn off default configs\n"
               "   -p <PROMPT>    - Choose the prompt text\n"
               "   -P <SUFFIX>    - Add a pkg-config suffix\n"
               "   -r <ROOT>      - Choose the root directory\n"
               "   -v             - Print the version\n";
}

inline void
print_error_usage ()
{
  print_usage (std::cerr);

  std::cerr << "Run cenv -h to get the possible options\n";
}

int
main (int argc,
      char **argv)
{
  cenv::config cfg;
  bool default_configs = true;

  int opt;
  while ((opt = getopt (argc, argv, "+:D:e:E:hi:I:l:m:np:P:r:v")) != -1)
    switch (opt)
      {
      case 'D':
        {
          char *p = strchr (optarg, '=');

          if (!p)
            {
              print_error_usage ();
              std::cerr << "The argument to -D should contain a key and a "
                           "value\n";
              return 2;
            }

          cfg.variables[std::string {optarg, p}] = std::string {p + 1};
          break;
        }

      case 'e':
        cfg.executable_suffixes.push_front (optarg);
        break;

      case 'E':
        {
          char *p = strchr (optarg, '=');

          if (!p)
            {
              print_error_usage ();
              std::cerr << "The argument to -E should contain a key and a "
                           "value\n";
              return 2;
            }

          cfg.variables[std::string {optarg, p}] = std::string {p + 1};
          break;
        }

      case 'h':
        print_help ();
        return 0;

      case 'i':
        cfg.include_suffixes.push_front (optarg);
        break;

      case 'I':
        cfg.info_suffixes.push_front (optarg);
        break;

      case 'l':
        cfg.library_suffixes.push_front (optarg);
        break;

      case 'm':
        cfg.manpage_suffixes.push_front (optarg);
        break;

      case 'n':
        default_configs = false;
        break;

      case 'p':
        cfg.prompt = optarg;
        cfg.prompt_set = true;
        break;

      case 'P':
        cfg.pkg_config_suffixes.push_front (optarg);
        break;

      case 'r':
        cfg.root = optarg;
        cfg.root_set = true;
        break;

      case 'v':
        std::cout << VERSION << '\n';
        return 0;

      case '?':
        print_error_usage ();
        std::cerr << "Unknown option -" << (char) optopt << '\n';
        return 2;

      case ':':
        print_error_usage ();
        std::cerr << "Missing argument for option -" << (char) optopt << '\n';
        return 2;

      default:
        abort ();
      }

  if (optind != (argc - 1))
    {
      print_error_usage ();
      std::cerr << "Exactly one folder name is required\n";
      return 2;
    }

  cfg.folder = argv[optind];

  if (mkdir (cfg.folder.c_str (), 0755))
    {
      int errno_save = errno;

      if (errno_save != EEXIST)
        {
          std::cerr << "Creating the directory " << cfg.folder
                    << " failed: " << std::strerror (errno_save) << '\n';
          return 1;
        }
    }

  char *folder_res = realpath (cfg.folder.c_str (), nullptr);
  cfg.folder = folder_res;
  free (folder_res);

  if (default_configs)
    cfg.add_default_configs ();

  {
    std::ostringstream activate_path_builder;
    activate_path_builder << cfg.folder << "/activate";
    std::ofstream out;
    out.exceptions (std::ios::badbit);
    out.open (activate_path_builder.str ());
    cfg.write_activate_script (out);
    out.close ();
  }

  return 0;
}
