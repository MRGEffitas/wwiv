/**************************************************************************/
/*                                                                        */
/*                              WWIV Version 5.x                          */
/*             Copyright (C)1998-2017, WWIV Software Services             */
/*                                                                        */
/*    Licensed  under the  Apache License, Version  2.0 (the "License");  */
/*    you may not use this  file  except in compliance with the License.  */
/*    You may obtain a copy of the License at                             */
/*                                                                        */
/*                http://www.apache.org/licenses/LICENSE-2.0              */
/*                                                                        */
/*    Unless  required  by  applicable  law  or agreed to  in  writing,   */
/*    software  distributed  under  the  License  is  distributed on an   */
/*    "AS IS"  BASIS, WITHOUT  WARRANTIES  OR  CONDITIONS OF ANY  KIND,   */
/*    either  express  or implied.  See  the  License for  the specific   */
/*    language governing permissions and limitations under the License.   */
/*                                                                        */
/**************************************************************************/
#include "bbs/menu.h"
#include <cstdint>
#include <memory>
#include <string>

#include "bbs/input.h"
#include "bbs/com.h"
#include "bbs/common.h"
#include "bbs/instmsg.h"
#include "bbs/menuedit.h"
#include "bbs/menusupp.h"
#include "bbs/menu_parser.h"
#include "bbs/mmkey.h"
#include "bbs/newuser.h"
#include "bbs/pause.h"
#include "bbs/printfile.h"
#include "bbs/sysoplog.h"
#include "bbs/bbs.h"
#include "bbs/bbsutl.h"
#include "bbs/utility.h"
#include "bbs/vars.h"
#include "core/stl.h"
#include "core/strings.h"
#include "core/textfile.h"
#include "core/wwivassert.h"
#include "sdk/filenames.h"

using std::string;
using std::unique_ptr;
using namespace wwiv::sdk;
using namespace wwiv::strings;
using namespace wwiv::stl;

namespace wwiv {
namespace menus {


// Local function prototypes
bool ValidateMenuSet(const char *pszMenuDir);
void StartMenus();
bool CheckMenuSecurity(const MenuHeader* pHeader, bool bCheckPassword);
void MenuExecuteCommand(MenuInstanceData* menu_data, const string& command);
void LogUserFunction(const MenuInstanceData* menu_data, const string& command, MenuRec* pMenu);
void PrintMenuPrompt(MenuInstanceData* menu_data);
const string GetCommand(const MenuInstanceData* menu_data);
bool CheckMenuItemSecurity(MenuRec* pMenu, bool bCheckPassword);
void InterpretCommand(MenuInstanceData* menu_data, const std::string& script);

static bool CheckMenuPassword(const string& original_password) {
  const string expected_password = (original_password == "*SYSTEM") 
      ? a()->config()->config()->systempw : original_password;
  bout.nl();
  string actual_password = input_password("|#2SY: ", 20);
  return actual_password == expected_password;
}

void mainmenu() {
  while (true) {
    StartMenus();
  }
}

void StartMenus() {
  unique_ptr<MenuInstanceData> menu_data(new MenuInstanceData());
  menu_data->reload = true;                    // force loading of menu
  while (menu_data->reload) {
    CheckForHangup();
    menu_data->finished = false;
    menu_data->reload = false;
    if (!ValidateMenuSet(a()->user()->data.szMenuSet)) {
      ConfigUserMenuSet();
    }
    menu_data->Menus(a()->user()->data.szMenuSet, "main"); // default starting menu
  }
}

void MenuInstanceData::Menus(const string& menuDirectory, const string& menuName) {
  path_ = menuDirectory;
  menu_ = menuName;

  if (Open()) {
    if (header.nums == MENU_NUMFLAG_DIRNUMBER && a()->udir[0].subnum == -1) {
      bout << "\r\nYou cannot currently access the file section.\r\n\n";
      Close();
      return;
    }
    // if flagged to display help on entrance, then do so
    if (a()->user()->IsExpert() && header.nForceHelp == MENU_HELP_ONENTRANCE) {
      DisplayHelp();
    }

    while (!finished) {
      PrintMenuPrompt(this);
      MenuExecuteCommand(this, GetCommand(this));
    }
  } else if (IsEqualsIgnoreCase(menuName.c_str(), "main")) {     // default menu name
    Hangup();
  }
  Close();
}

MenuInstanceData::MenuInstanceData() {}

MenuInstanceData::~MenuInstanceData() {
  Close();
}

void MenuInstanceData::Close() {
  insertion_order_.clear();
  menu_command_map_.clear();
}

//static
const std::string MenuInstanceData::create_menu_filename(
    const std::string& path, const std::string& menu, const std::string& extension) {
  return StrCat(GetMenuDirectory(), path, File::pathSeparatorString, menu, ".", extension);
}

const string MenuInstanceData::create_menu_filename(const string& extension) const {
  return MenuInstanceData::create_menu_filename(path_, menu_, extension);
}

bool MenuInstanceData::CreateMenuMap(File* menu_file) {
  insertion_order_.clear();
  auto nAmount = menu_file->GetLength() / sizeof(MenuRec);

  for (size_t nRec = 1; nRec < nAmount; nRec++) {
    MenuRec menu;
    menu_file->Seek(nRec * sizeof(MenuRec), File::Whence::begin);
    menu_file->Read(&menu, sizeof(MenuRec));

    menu_command_map_.emplace(menu.szKey, menu);
    if (nRec != 0 && !(menu.nFlags & MENU_FLAG_DELETED)) {
      insertion_order_.push_back(menu.szKey);
    }
  }
  return true;
}

bool MenuInstanceData::Open() {
  Close();

  // Open up the main data file
  unique_ptr<File> menu_file(new File(create_menu_filename("mnu")));
  if (!menu_file->Open(File::modeBinary | File::modeReadOnly, File::shareDenyNone))  {
    // Unable to open menu
    MenuSysopLog("Unable to open Menu");
    return false;
  }

  // Read the header (control) record into memory
  menu_file->Seek(0L, File::Whence::begin);
  menu_file->Read(&header, sizeof(MenuHeader));

  // Version numbers can be checked here.
  if (!CreateMenuMap(menu_file.get())) {
    MenuSysopLog("Unable to create menu index.");
    return false;
  }

  if (!CheckMenuSecurity(&header, true)) {
    MenuSysopLog("< Menu Sec");
    return false;
  }

  // Open/Rease/Close Prompt file.  We use binary mode since we want the
  // \r to remain on windows (and linux).
  TextFile prompt_file(create_menu_filename("pro"), "rb");
  if (prompt_file.IsOpen()) {
    string tmp = prompt_file.ReadFileIntoString();
    string::size_type end = tmp.find(".end.");
    if (end != string::npos) {
      prompt = tmp.substr(0, end);
    } else {
      prompt = tmp;
    }
  }

  // Execute command to use on entering the menu (if any).
  if (header.szScript[0]) {
    InterpretCommand(this, header.szScript);
  }
  return true;
}

static const string GetHelpFileName(const MenuInstanceData* menu_data) {
  if (a()->user()->HasAnsi()) {
    if (a()->user()->HasColor()) {
      const string filename = menu_data->create_menu_filename("ans");
      if (File::Exists(filename)) {
        return filename;
      }
    }
    const string filename = menu_data->create_menu_filename("b&w");
    if (File::Exists(filename)) {
      return filename;
    }
  }
  return menu_data->create_menu_filename("msg");
}

void MenuInstanceData::DisplayHelp() const {
  const string filename = GetHelpFileName(this);
  if (!printfile(filename, true)) {
    GenerateMenu();
  }
}

bool CheckMenuSecurity(const MenuHeader* pHeader, bool bCheckPassword) {
  if ((pHeader->nFlags & MENU_FLAG_DELETED) ||
      (a()->GetEffectiveSl() < pHeader->nMinSL) ||
      (a()->user()->GetDsl() < pHeader->nMinDSL)) {
    return false;
  }

  // All AR bits specified must match
  for (short int x = 0; x < 16; x++) {
    if (pHeader->uAR & (1 << x)) {
      if (!a()->user()->HasArFlag(1 << x)) {
        return false;
      }
    }
  }

  // All DAR bits specified must match
  for (short int x = 0; x < 16; x++) {
    if (pHeader->uDAR & (1 << x)) {
      if (!a()->user()->HasDarFlag(1 << x)) {
        return (a()->user()->GetDsl() < pHeader->nMinDSL);
      }
    }
  }

  // If any restrictions match, then they arn't allowed
  for (short int x = 0; x < 16; x++) {
    if (pHeader->uRestrict & (1 << x)) {
      if (a()->user()->HasRestrictionFlag(1 << x)) {
        return (a()->user()->GetDsl() < pHeader->nMinDSL);
      }
    }
  }

  if ((pHeader->nSysop && !so()) ||
      (pHeader->nCoSysop && !cs())) {
    return false;
  }

  if (pHeader->szPassWord[0] && bCheckPassword) {
    if (!CheckMenuPassword(pHeader->szPassWord)) {
      return false;
    }
  }
  return true;
}

static bool IsNumber(const string& command) {
  if (!command.length()) {
    return false;
  }
  for (const auto& ch : command) {
    if (!isdigit(ch)) {
      return false;
    }
  }
  return true;
}

bool MenuInstanceData::LoadMenuRecord(const std::string& command, MenuRec* pMenu) {
  // If we have 'numbers set the sub #' turned on then create a command to do so if a # is entered.
  if (IsNumber(command)) {
    if (header.nums == MENU_NUMFLAG_SUBNUMBER) {
      memset(pMenu, 0, sizeof(MenuRec));
      sprintf(pMenu->szExecute, "SetSubNumber %d", atoi(command.c_str()));
      return true;
    } else if (header.nums == MENU_NUMFLAG_DIRNUMBER) {
      memset(pMenu, 0, sizeof(MenuRec));
      sprintf(pMenu->szExecute, "SetDirNumber %d", atoi(command.c_str()));
      return true;
    }
  }

  if (contains(menu_command_map_, command)) {
    *pMenu = menu_command_map_.at(command);
    if (CheckMenuItemSecurity(pMenu, true)) {
      return true;
    }
    MenuSysopLog(StrCat("|06< item security : ", command));
  }
  return false;
}

void MenuExecuteCommand(MenuInstanceData* menu_data, const string& command) {
  MenuRec menu;
  if (menu_data->LoadMenuRecord(command, &menu)) {
    LogUserFunction(menu_data, command, &menu);
    InterpretCommand(menu_data, menu.szExecute);
  } else {
    LogUserFunction(menu_data, command, &menu);
  }
}

void LogUserFunction(const MenuInstanceData* menu_data, const string& command, MenuRec* pMenu) {
  switch (menu_data->header.nLogging) {
  case MENU_LOGTYPE_KEY:
    sysopchar(command);
    break;
  case MENU_LOGTYPE_COMMAND:
    sysoplog() << pMenu->szExecute;
    break;
  case MENU_LOGTYPE_DESC:
    sysoplog() << (pMenu->szMenuText[0] ? pMenu->szMenuText : pMenu->szExecute);
    break;
  case MENU_LOGTYPE_NONE:
  default:
    break;
  }
}

void MenuSysopLog(const string& msg) {
  const string log_message = StrCat("*MENU* : ", msg);
  sysoplog() << log_message;
  bout << log_message << wwiv::endl;
}

void PrintMenuPrompt(MenuInstanceData* menu_data) {
  if (!a()->user()->IsExpert() || menu_data->header.nForceHelp == MENU_HELP_FORCE) {
    menu_data->DisplayHelp();
  }
  TurnMCIOn();
  if (!menu_data->prompt.empty()) {
    bout << menu_data->prompt;
  }
}

void TurnMCIOff() {
  g_flags |= g_flag_disable_mci;
}

void TurnMCIOn() {
  g_flags &= ~g_flag_disable_mci;
}

void ConfigUserMenuSet() {
  bout.cls();
  bout.litebar("Configure Menus");
  printfile(MENUWEL_NOEXT);
  bool done = false;
  while (!done) {
    bout.nl();
    bout << "|#11|#9) Menuset      :|#2 " << a()->user()->data.szMenuSet << wwiv::endl;
    bout << "|#12|#9) Use hot keys :|#2 " << (a()->user()->data.cHotKeys == HOTKEYS_ON ? "Yes" : "No ")
         << wwiv::endl;
    bout.nl();
    bout << "|#9(|#2Q|#9=|#1Quit|#9) : ";
    char chKey = onek("Q12?");

    switch (chKey) {
    case 'Q':
      done = true;
      break;
    case '1': {
      ListMenuDirs();
      bout.nl(2);
      bout << "|#9Enter the menu set to use : ";
      string menuSetName = inputl(8);
      if (ValidateMenuSet(menuSetName.c_str())) {
        wwiv::menus::MenuDescriptions descriptions(GetMenuDirectory());
        bout.nl();
        bout << "|#9Menu Set : |#2" <<  menuSetName.c_str() << " :  |#1" << descriptions.description(menuSetName) << wwiv::endl;
        bout << "|#5Use this menu set? ";
        if (noyes()) {
          strcpy(a()->user()->data.szMenuSet, menuSetName.c_str());
          break;
        }
      }
      bout.nl();
      bout << "|#6That menu set does not exists, resetting to the default menu set" << wwiv::endl;
      pausescr();
      if (a()->user()->data.szMenuSet[0] == '\0') {
        strcpy(a()->user()->data.szMenuSet, "wwiv");
      }
    }
    break;
    case '2':
      a()->user()->data.cHotKeys = !a()->user()->data.cHotKeys;
      break;
    case '?':
      printfile(MENUWEL_NOEXT);
      continue;                           // bypass the below cls()
    }
    bout.cls();
  }

  // If menu is invalid, it picks the first one it finds
  if (!ValidateMenuSet(a()->user()->data.szMenuSet)) {
    if (a()->languages.size() > 1 && a()->user()->GetLanguage() != 0) {
      bout << "|#6No menus for " << a()->languages[a()->user()->GetLanguage()].name
           << " language.";
      input_language();
    }
  }

  // Save current menu setup.
  a()->WriteCurrentUser();

  MenuSysopLog(StringPrintf("Menu in use : %s - %s", a()->user()->data.szMenuSet,
          a()->user()->data.cHotKeys == HOTKEYS_ON ? "Hot" : "Off"));
  bout.nl(2);
}

bool ValidateMenuSet(const char *pszMenuDir) {
  // ensure the entry point exists
  return File::Exists(GetMenuDirectory(pszMenuDir), "main.mnu");
}

const string GetCommand(const MenuInstanceData* menu_data) {
  if (a()->user()->data.cHotKeys == HOTKEYS_ON) {
    if (menu_data->header.nums == MENU_NUMFLAG_DIRNUMBER) {
      write_inst(INST_LOC_XFER, a()->current_user_dir().subnum, INST_FLAGS_NONE);
      return string(mmkey(1));
    } else if (menu_data->header.nums == MENU_NUMFLAG_SUBNUMBER) {
      write_inst(INST_LOC_MAIN, a()->current_user_sub().subnum, INST_FLAGS_NONE);
      return string(mmkey(0));
    } else {
      std::set<char> odc = {'/'};
      return string(mmkey(odc));
    }
  } else {
    return input(50);
  }
}

bool CheckMenuItemSecurity(MenuRec * pMenu, bool bCheckPassword) {
  // if deleted, return as failed
  if ((pMenu->nFlags & MENU_FLAG_DELETED) ||
      (a()->GetEffectiveSl() < pMenu->nMinSL) ||
      (a()->GetEffectiveSl() > pMenu->iMaxSL && pMenu->iMaxSL != 0) ||
      (a()->user()->GetDsl() < pMenu->nMinDSL) ||
      (a()->user()->GetDsl() > pMenu->iMaxDSL && pMenu->iMaxDSL != 0)) {
    return false;
  }

  // All AR bits specified must match
  for (int x = 0; x < 16; x++) {
    if (pMenu->uAR & (1 << x)) {
      if (!a()->user()->HasArFlag(1 << x)) {
        return false;
      }
    }
  }

  // All DAR bits specified must match
  for (int x = 0; x < 16; x++) {
    if (pMenu->uDAR & (1 << x)) {
      if (!a()->user()->HasDarFlag(1 << x)) {
        return false;
      }
    }
  }

  // If any restrictions match, then they arn't allowed
  for (int x = 0; x < 16; x++) {
    if (pMenu->uRestrict & (1 << x)) {
      if (a()->user()->HasRestrictionFlag(1 << x)) {
        return false;
      }
    }
  }

  if ((pMenu->nSysop && !so()) ||
      (pMenu->nCoSysop && !cs())) {
    return false;
  }

  if (pMenu->szPassWord[0] && bCheckPassword) {
    if (!CheckMenuPassword(pMenu->szPassWord)) {
      return false;
    }
  }

  // If you made it past all of the checks
  // then you may execute the menu record
  return true;
}

MenuDescriptions::MenuDescriptions(const std::string& menupath) :menupath_(menupath) {
  TextFile file(menupath, DESCRIPT_ION, "rt");
  if (file.IsOpen()) {
    string s;
    while (file.ReadLine(&s)) {
      StringTrim(&s);
      if (s.empty()) {
        continue;
      }
      string::size_type space = s.find(' ');
      if (space == string::npos) {
        continue;
      }
      string menu_name = s.substr(0, space);
      string description = s.substr(space + 1);
      StringLowerCase(&menu_name);
      StringLowerCase(&description);
      descriptions_.emplace(menu_name, description);
    }
  }
}

MenuDescriptions::~MenuDescriptions() {}

const std::string MenuDescriptions::description(const std::string& name) const {
  if (contains(descriptions_, name)) {
    return descriptions_.at(name);
  }
  return "";
}

bool MenuDescriptions::set_description(const std::string& name, const std::string& description) {
  descriptions_[name] = description;

  TextFile file(menupath_, DESCRIPT_ION, "wt");
  if (!file.IsOpen()) {
    return false;
  }

  for (const auto& iter : descriptions_) {
    file.WriteFormatted("%s %s", iter.first.c_str(), iter.second.c_str());
  }
  return true;
}

const string GetMenuDirectory() {
  return StrCat(a()->language_dir, "menus", File::pathSeparatorString);
}

const string GetMenuDirectory(const string menuPath) {
  return StrCat(GetMenuDirectory(), menuPath, File::pathSeparatorString);
}

void MenuInstanceData::GenerateMenu() const {
  bout.Color(0);
  bout.nl();

  int iDisplayed = 0;
  if (header.nums != MENU_NUMFLAG_NOTHING) {
    bout.bprintf("|#1%-8.8s  |#2%-25.25s  ", "[#]", "Change Sub/Dir #");
    ++iDisplayed;
  }
  for (const auto& key : insertion_order_) {
    if (!contains(menu_command_map_, key)) {
      continue;
    }
    MenuRec menu = menu_command_map_.at(key);
    if (CheckMenuItemSecurity(&menu, false) &&
        menu.nHide != MENU_HIDE_REGULAR &&
        menu.nHide != MENU_HIDE_BOTH) {
      char szKey[30];
      if (strlen(menu.szKey) > 1 && menu.szKey[0] != '/' && a()->user()->data.cHotKeys == HOTKEYS_ON) {
        sprintf(szKey, "//%s", menu.szKey);
      } else {
        sprintf(szKey, "[%s]", menu.szKey);
      }
      bout.bprintf("|#1%-8.8s  |#2%-25.25s  ", szKey,
                    menu.szMenuText[0] ? menu.szMenuText : menu.szExecute);
      if (iDisplayed % 2) {
        bout.nl();
      }
      ++iDisplayed;
    }
  }
  if (IsEquals(a()->user()->GetName(), "GUEST")) {
    if (iDisplayed % 2) {
      bout.nl();
    }
    bout.bprintf("|#1%-8.8s  |#2%-25.25s  ",
      a()->user()->data.cHotKeys == HOTKEYS_ON ? "//APPLY" : "[APPLY]",
      "Guest Account Application");
    ++iDisplayed;
  }
  bout.nl(2);
  return;
}

}  // namespace menus
}  // namespace wwiv
