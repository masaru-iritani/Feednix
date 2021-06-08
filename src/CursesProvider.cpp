#include <curses.h>
#include <filesystem>
#include <map>
#include <panel.h>
#include <menu.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <json/json.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "CursesProvider.h"

constexpr auto POSTS_STATUSLINE = "Enter: See Preview  A: mark all read  u: mark unread  r: mark read  = : change sort type s: mark saved  S: mark unsaved R: refresh  o: Open in plain-text  O: Open in Browser  F1: exit";
constexpr auto CTG_STATUSLINE = "Enter: Fetch Stream  A: mark all read  R: refresh  F1: exit";

#define HOME_PATH getenv("HOME")

namespace fs = std::filesystem;
using namespace std::literals::string_literals;
using PipeStream = std::unique_ptr<FILE, decltype(&pclose)>;

static inline void throwIfError(int result, const std::string& action){
        if(result == ERR){
                throw std::runtime_error("Failed to " + action);
        }
}

static inline unsigned int u(int x){
        return (x > 0) ? static_cast<unsigned int>(x) : 0U;
}

CursesProvider::CursesProvider(const fs::path& tmpPath, bool verbose, bool change):
        feedly{FeedlyProvider(tmpPath)},
        previewPath{tmpPath / "preview.html"}{

        feedly.setVerbose(verbose);
        feedly.setChangeTokensFlag(change);
        feedly.authenticateUser();

        setlocale(LC_ALL, "");
        initscr();

        start_color();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);

        feedly.setVerbose(false);
}
void CursesProvider::init(){
        Json::Value root;
        Json::Reader reader;

        if(const auto browserEnv = getenv("BROWSER")){
                textBrowser = browserEnv;
        }

        std::ifstream tokenFile(std::string(std::string(HOME_PATH) + "/.config/feednix/config.json").c_str(), std::ifstream::binary);
        if(reader.parse(tokenFile, root)){
                init_pair(1, root["colors"]["active_panel"].asInt(), root["colors"]["background"].asInt());
                init_pair(2, root["colors"]["idle_panel"].asInt(), root["colors"]["background"].asInt());
                init_pair(3, root["colors"]["counter"].asInt(), root["colors"]["background"].asInt());
                init_pair(4, root["colors"]["status_line"].asInt(), root["colors"]["background"].asInt());
                init_pair(5, root["colors"]["instructions_line"].asInt(), root["colors"]["background"].asInt());
                init_pair(6, root["colors"]["item_text"].asInt(), root["colors"]["background"].asInt());
                init_pair(7, root["colors"]["item_highlight"].asInt(), root["colors"]["background"].asInt());
                init_pair(8, root["colors"]["read_item"].asInt(), root["colors"]["background"].asInt());

                ctgWinWidth = root["ctg_win_width"].asInt();
                viewWinHeight = root["view_win_height"].asInt();
                viewWinHeightPer = root["view_win_height_per"].asInt();

                currentRank = root["rank"].asBool();
                secondsToMarkAsRead = std::chrono::seconds(root["seconds_to_mark_as_read"].asInt());

                if(textBrowser.empty()){
                        textBrowser = root["text_browser"].asString();
                }
        }
        else{
                endwin();
                feedly.curl_cleanup();
                std::cerr << "ERROR: Couldn't not read config file" << std::endl;
                exit(EXIT_FAILURE);
        }

        if(textBrowser.empty()){
                textBrowser = "w3m";
        }

        if(textBrowser.at(0) == '~'){
                textBrowser.replace(0, 1, getenv("HOME"));
        }

        calculateWindowSizes();
        statusWin = newwin(statusWinSize.height, statusWinSize.width, statusWinSize.y, statusWinSize.x);
        infoWin = newwin(infoWinSize.height, infoWinSize.width, infoWinSize.y, infoWinSize.x);
        viewWin = newwin(viewWinSize.height, viewWinSize.width, viewWinSize.y, viewWinSize.x);
        createCategoriesMenu();
        createPostsMenu();

        ctgPanel = new_panel(ctgWin);
        postsPanel = new_panel(postsWin);
        viewPanel = new_panel(viewWin);
        statusPanel = new_panel(statusWin);
        infoPanel = new_panel(infoWin);

        printPostMenuMessage("Loading...");
        updateInfoLine(POSTS_STATUSLINE);

        update_panels();
        doupdate();

        ctgMenuCallback("All");
}
void CursesProvider::control(){
        int ch;
        auto curMenu = postsMenu;
        if(totalPosts > 0){
                // Preview the first post.
                changeSelectedItem(curMenu, REQ_FIRST_ITEM);
        }

        while((ch = getch()) != KEY_F(1) && ch != 'q'){
                auto curItem = current_item(curMenu);
                switch(ch){
                        case 10: // Enter
                                if((curMenu == ctgMenu) && (curItem != NULL)){
                                        updateStatusLine("[Updating stream]", "", false);
                                        refresh();
                                        update_panels();

                                        ctgMenuCallback(item_name(curItem));

                                        if(numUnread == 0){
                                                curMenu = ctgMenu;
                                        }
                                        else{
                                                curMenu = postsMenu;
                                                updateInfoLine(POSTS_STATUSLINE);
                                        }
                                }
                                else if((curMenu == postsMenu) && (curItem != NULL)){
                                        postsMenuCallback(curItem, true /*preview*/);
                                }

                                break;
                        case 9: // Tab
                                if(curMenu == ctgMenu){
                                        curMenu = postsMenu;
                                        renderWindow(postsWin, "Posts", true);
                                        renderWindow(ctgWin, "Categories", false);
                                        updateInfoLine(POSTS_STATUSLINE);
                                }
                                else{
                                        curMenu = ctgMenu;
                                        renderWindow(ctgWin, "Categories", true);
                                        renderWindow(postsWin, "Posts", false);
                                        updateInfoLine(CTG_STATUSLINE);
                                }

                                refresh();
                                break;
                        case KEY_RESIZE:{
                                const auto m = [](PANEL* const panel, const WindowSize& r){
                                        // Shrink the window tentatively to prevent the window
                                        // from going beyond the screen boundary.
                                        if(const auto window = panel_window(panel)){
                                                if((r.width * r.height) > 0U){
                                                        throwIfError(wresize(window, 1, 1), "minimize a window");
                                                        throwIfError(replace_panel(panel, window), "replace the window for a panel");

                                                        const auto moveErrorMessage = "move a panel to " + std::to_string(r.y) + ", " + std::to_string(r.x) + " (COLS = " + std::to_string(COLS) + ", LINES = " + std::to_string(LINES) + ")";
                                                        throwIfError(move_panel(panel, r.y, r.x), moveErrorMessage);

                                                        const auto resizeErrorMessage = "resize a window to " + std::to_string(r.height) + ", " + std::to_string(r.width);
                                                        throwIfError(wresize(window, r.height, r.width), resizeErrorMessage);
                                                        throwIfError(replace_panel(panel, window), "replace the window for a panel");
                                                        throwIfError(show_panel(panel), "show a panel");
                                                }
                                                else{
                                                        throwIfError(hide_panel(panel), "hide a panel");
                                                }
                                        }
                                };

                                calculateWindowSizes();
                                m(ctgPanel, ctgWinSize);
                                m(infoPanel, infoWinSize);
                                m(postsPanel, postsWinSize);
                                m(statusPanel, statusWinSize);
                                m(viewPanel, viewWinSize);
                                updateStatusLine("[Updated the screen size]", "", false);
                                refreshInfoLine();
                                //throwIfError(unpost_menu(postsMenu), "hide the menu");
                                //throwIfError(set_menu_format(postsMenu, postsMenuWinSize.height, postsMenuWinSize.width), "resize the menu");
                                //throwIfError(post_menu(postsMenu), "show the menu");
                                renderWindow(postsWin, "Posts", curMenu == postsMenu);
                                throwIfError(unpost_menu(ctgMenu), "hide the menu");
                                throwIfError(set_menu_format(ctgMenu, ctgMenuWinSize.height, ctgMenuWinSize.width), "resize the menu");
                                throwIfError(post_menu(ctgMenu), "show the menu");
                                renderWindow(ctgWin, "Categories", curMenu == ctgMenu);
                                throwIfError(wrefresh(ctgWin), "refresh the category window");
                                throwIfError(wrefresh(ctgMenuWin), "refresh the category menu window");
                                throwIfError(refresh(), "refresh the screen");
                                break;
                        }
                        case '=':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        updateStatusLine("[Updating stream]", "", false);
                                        refresh();

                                        currentRank = !currentRank;

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                }

                                break;
                        case KEY_DOWN:
                                changeSelectedItem(curMenu, REQ_DOWN_ITEM);
                                break;
                        case KEY_UP:
                                changeSelectedItem(curMenu, REQ_UP_ITEM);
                                break;
                        case 'j':
                                changeSelectedItem(curMenu, REQ_DOWN_ITEM);
                                break;
                        case 'k':
                                changeSelectedItem(curMenu, REQ_UP_ITEM);
                                break;
                        case 'u':
                                if((curMenu == postsMenu) && (curItem != NULL) && !item_opts(curItem)){
                                        updateStatusLine("[Marking post unread]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsUnread({item_description(curItem)});

                                                item_opts_on(curItem, O_SELECTABLE);
                                                numUnread++;
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        updateStatusLine(errorMessage, "", errorMessage.empty());

                                        // Prevent an article marked as unread explicitly
                                        // from being marked as read automatically.
                                        lastPostSelectionTime = std::chrono::time_point<std::chrono::steady_clock>::max();
                                }

                                break;
                        case 'r':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        markItemRead(curItem);
                                }

                                break;
                        case 's':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        updateStatusLine("[Marking post saved]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsSaved({item_description(curItem)});
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        updateStatusLine(errorMessage, "", errorMessage.empty());
                                }

                                break;
                        case 'S':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        updateStatusLine("[Marking post Unsaved]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markPostsUnsaved({item_description(curItem)});
                                        }
                                        catch(const std::exception& e){
                                                errorMessage = e.what();
                                        }

                                        updateStatusLine(errorMessage, "", errorMessage.empty());
                                }

                                break;
                        case 'R':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        updateStatusLine("[Updating stream]", "", false);
                                        refresh();

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                }

                                break;
                        case 'o':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        postsMenuCallback(curItem, false /*preview*/);
                                }

                                break;
                        case 'O':
                                if((curMenu == postsMenu) && (curItem != NULL)){
                                        termios oldt;
                                        tcgetattr(STDIN_FILENO, &oldt);
                                        termios newt = oldt;
                                        newt.c_lflag &= ~ECHO;
                                        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

                                        try{
                                                PostData& data = feedly.getSinglePostData(item_index(curItem));
#ifdef __APPLE__
                                                execute("open", data.originURL);
#else
                                                execute("xdg-open", data.originURL);
#endif
                                                markItemRead(curItem);
                                        }
                                        catch(const std::exception& e){
                                                updateStatusLine(e.what(), "" /*post*/, false /*showCounter*/);
                                        }
                                }

                                break;
                        case 'a':
                                {
                                        char feed[200];
                                        char title[200];
                                        char ctg[200];
                                        echo();

                                        clearStatusLine();
                                        attron(COLOR_PAIR(4));
                                        mvprintw(LINES - 2, 0, "[ENTER FEED]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER FEED]") + 1, feed, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        clrtoeol();

                                        mvprintw(LINES - 2, 0, "[ENTER TITLE]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER TITLE]") + 1, title, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        clrtoeol();

                                        mvprintw(LINES - 2, 0, "[ENTER CATEGORY]:");
                                        mvgetnstr(LINES-2, strlen("[ENTER CATEGORY]") + 1, ctg, 200);
                                        mvaddch(LINES-2, 0, ':');

                                        std::istringstream ss(ctg);
                                        std::istream_iterator<std::string> begin(ss), end;

                                        std::vector<std::string> arrayTokens(begin, end);

                                        noecho();
                                        clrtoeol();

                                        updateStatusLine("[Adding subscription]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        if(strlen(feed) != 0){
                                                try{
                                                        feedly.addSubscription(false, feed, arrayTokens, title);
                                                }
                                                catch(const std::exception& e){
                                                        errorMessage = e.what();
                                                }
                                        }

                                        updateStatusLine(errorMessage, "", errorMessage.empty());
                                }

                                break;
                        case 'A':
                                if(auto currentCategoryItem = current_item(ctgMenu)){
                                        wclear(viewWin);
                                        updateStatusLine("[Marking category read]", "", true);
                                        refresh();

                                        std::string errorMessage;
                                        try{
                                                feedly.markCategoriesRead(item_description(currentCategoryItem), lastEntryRead);
                                        }
                                        catch(const std::exception& e){
                                                updateStatusLine(errorMessage, "", errorMessage.empty());
                                        }

                                        ctgMenuCallback(item_name(currentCategoryItem));
                                        curMenu = ctgMenu;
                                }

                                break;
                         default:{
                                updateStatusLine("[Unsupported key binding: " + std::to_string(ch) + "]", "", false /*showCounter*/);
                                break;
                         }
                }

                update_panels();
                doupdate();
        }

        markItemReadAutomatically(current_item(postsMenu));
}
void CursesProvider::calculateWindowSizes(){
        const auto actualInfoWinHeight = (LINES >= 1) ? 1U : 0U;
        infoWinSize = {0U, u(LINES - 1), u(COLS), actualInfoWinHeight};

        const auto actualStatusWinHeight = (LINES >= 2) ? 1U : 0U;
        statusWinSize = {0U, u(LINES - 2), u(COLS), actualStatusWinHeight};

        const auto minCtgWinHeight = 5U;
        const auto maxViewWinHeight = u(LINES - static_cast<int>(minCtgWinHeight + actualStatusWinHeight + actualInfoWinHeight));
        auto desiredViewWinHeight = 0U;
        if(viewWinHeight >= 0){
                // Use the specified view window height if available.
                desiredViewWinHeight = u(viewWinHeight);
        }
        else if((viewWinHeightPer >= 0) && (viewWinHeightPer <= 100)){
                // Use the specified view window height ratio if available.
                desiredViewWinHeight = u((LINES - 2) * viewWinHeightPer / 100);
        }
        else{
                // Use the default view window height ratio.
                desiredViewWinHeight = u((LINES - 2) * VIEW_WIN_HEIGHT_PER / 100);
        }

        const auto actualViewWinHeight = std::min(desiredViewWinHeight, maxViewWinHeight);
        viewWinSize = {0U, u(LINES - actualViewWinHeight), u(COLS), actualViewWinHeight};

        constexpr auto minCtgWinWidth = 3U;
        constexpr auto minPostsWinWidth = 3U;
        const auto maxCtgWinWidth = u(COLS - minPostsWinWidth);
        const auto desiredCtgWinWidth = (ctgWinWidth >= 0) ? u(ctgWinWidth) : CTG_WIN_WIDTH;
        const auto actualCtgWinWidth = (minCtgWinWidth > maxCtgWinWidth) ? 0U : std::clamp(desiredCtgWinWidth, minCtgWinWidth, maxCtgWinWidth);
        const auto desiredCtgWinHeight = u(LINES - static_cast<int>(viewWinSize.height + statusWinSize.height + infoWinSize.height));
        const auto actualCtgWinHeight = (desiredCtgWinHeight >= minCtgWinHeight) ? desiredCtgWinHeight : 0U;
        ctgWinSize = {0U, 0U, actualCtgWinWidth, actualCtgWinHeight};
        ctgMenuWinSize = {1U, 3U, u(actualCtgWinWidth - 2), u(actualCtgWinHeight - 4)};

        const auto desiredPostsWinWidth = u(COLS - actualCtgWinWidth);
        const auto actualPostsWinWidth = (desiredPostsWinWidth < minPostsWinWidth) ? 0U : desiredPostsWinWidth;
        postsWinSize = {u(actualCtgWinWidth), 0U, u(actualPostsWinWidth), u(actualCtgWinHeight)};
        postsMenuWinSize = {1U, 3U, u(actualPostsWinWidth - 2), u(actualCtgWinHeight - 4)};
}
void CursesProvider::createCategoriesMenu(){
        clearCategoryItems();
        try{
                const auto& labels = feedly.getLabels();
                ctgItems.push_back(new_item("All", labels.at("All").c_str()));
                ctgItems.push_back(new_item("Saved", labels.at("Saved").c_str()));
                ctgItems.push_back(new_item("Uncategorized", labels.at("Uncategorized").c_str()));
                for(const auto& [label, id] : labels){
                        if((label != "All") && (label != "Saved") && (label != "Uncategorized")){
                                ctgItems.push_back(new_item(label.c_str(), id.c_str()));
                        }
                }
        }
        catch(const std::exception& e){
                clearCategoryItems();
                updateStatusLine(e.what(), "" /*post*/, false /*showCounter*/);
        }

        ctgItems.push_back(NULL);
        ctgMenu = new_menu(ctgItems.data());

        ctgWin = newwin(ctgWinSize.height, ctgWinSize.width, ctgWinSize.y, ctgWinSize.x);
        ctgMenuWin = derwin(ctgWin, ctgMenuWinSize.height, ctgMenuWinSize.width, ctgMenuWinSize.y, ctgMenuWinSize.x);
        keypad(ctgWin, TRUE);

        set_menu_win(ctgMenu, ctgWin);
        set_menu_sub(ctgMenu, ctgMenuWin);
        set_menu_fore(ctgMenu, COLOR_PAIR(7) | A_REVERSE);
        set_menu_back(ctgMenu, COLOR_PAIR(6));
        set_menu_grey(ctgMenu, COLOR_PAIR(8));
        set_menu_mark(ctgMenu, "  ");

        renderWindow(ctgWin, "Categories", false);

        menu_opts_off(ctgMenu, O_SHOWDESC);
        menu_opts_on(ctgMenu, O_NONCYCLIC);

        post_menu(ctgMenu);
}
void CursesProvider::createPostsMenu(){
        postsWin = newwin(postsWinSize.height, postsWinSize.width, postsWinSize.y, postsWinSize.x);
        keypad(postsWin, TRUE);

        postsMenuWin = derwin(postsWin, postsWinSize.height - 4, postsWinSize.width - 2, 3, 1);
        postsMenu = new_menu(NULL);
        set_menu_win(postsMenu, postsWin);
        set_menu_sub(postsMenu, postsMenuWin);
        set_menu_fore(postsMenu, COLOR_PAIR(7) | A_REVERSE);
        set_menu_back(postsMenu, COLOR_PAIR(6));
        set_menu_grey(postsMenu, COLOR_PAIR(8));
        set_menu_mark(postsMenu, "*");

        renderWindow(postsWin, "Posts", true);

        menu_opts_off(postsMenu, O_SHOWDESC);

        post_menu(postsMenu);
}
void CursesProvider::ctgMenuCallback(const char* label){
        markItemReadAutomatically(current_item(postsMenu));

        std::string errorMessage;
        clearPostItems();
        try{
                const auto& posts = feedly.giveStreamPosts(label, currentRank);
                for(const auto& post : posts){
                        postsItems.push_back(new_item(post.title.c_str(), post.id.c_str()));
                }
        }
        catch(const std::exception& e){
                clearPostItems();
                errorMessage = e.what();
        }

        totalPosts = postsItems.size();
        numUnread = totalPosts;
        printPostMenuMessage("");

        postsItems.push_back(NULL);
        unpost_menu(postsMenu);
        set_menu_items(postsMenu, postsItems.data());
        set_menu_format(postsMenu, postsMenuWinSize.height, 0);
        post_menu(postsMenu);

        updateStatusLine(errorMessage, "", errorMessage.empty());
        renderWindow(postsWin, "Posts", true);
        renderWindow(ctgWin, "Categories", false);

        if(totalPosts > 0){
                lastEntryRead = item_description(postsItems.at(0));
                changeSelectedItem(postsMenu, REQ_FIRST_ITEM);
        }
        else
        {
                printPostMenuMessage("All Posts Read");
                wclear(viewWin);
        }
}
void CursesProvider::changeSelectedItem(MENU* curMenu, int req){
        ITEM* previousItem = current_item(curMenu);
        menu_driver(curMenu, req);
        ITEM* curItem = current_item(curMenu);

        if((curMenu != postsMenu) ||
            !curItem ||
            ((previousItem == curItem) && (req != REQ_FIRST_ITEM))){
                return;
        }

        markItemReadAutomatically(previousItem);

        try{
                const auto& postData = feedly.getSinglePostData(item_index(curItem));
                if(auto myfile = std::ofstream(previewPath.c_str())){
                        myfile << postData.content;
                }

                std::string content;
                char buffer[256];
                const auto command = "w3m -dump -cols " + std::to_string(COLS - 2) + " " + previewPath.native();
                if(const auto stream = PipeStream(popen(command.c_str(), "r"), &pclose)){
                        while(!feof(stream.get())){
                                if(fgets(buffer, 256, stream.get()) != NULL){
                                        content.append(buffer);
                                }
                        }
                }

                wclear(viewWin);
                mvwprintw(viewWin, 1, 1, content.c_str());
                wrefresh(viewWin);
                updateStatusLine("", postData.originTitle + " - " + postData.title, true /*showCounter*/);

                update_panels();
        }
        catch (const std::exception& e){
                updateStatusLine(e.what(), "" /*post*/, false /*showCounter*/);
        }
}
void CursesProvider::postsMenuCallback(ITEM* item, bool preview){
        auto command = std::string{};
        auto arg = std::string{};
        try{
                const auto& postData = feedly.getSinglePostData(item_index(item));
                if(preview){
                        if(auto myfile = std::ofstream(previewPath.c_str())){
                                myfile << postData.content;
                        }

                        command = "w3m";
                        arg = previewPath.native();
                }
                else{
                        command = textBrowser;
                        arg = postData.originURL;
                }

        }
        catch (const std::exception& e){
                updateStatusLine(e.what(), "" /*post*/, false /*showCounter*/);
                return;
        }

        const auto exitCode = execute(command, arg);
        if(exitCode == 0){
                markItemRead(item);
                lastEntryRead = item_description(item);
        }
        else{
                const auto updateStatus = preview ? "Failed to preview the post" : "Failed to open the post";
                updateStatusLine(updateStatus, "" /*post*/, false /*showCounter*/);
        }

        if(preview){
                auto errorCode = std::error_code{};
                fs::remove(previewPath, errorCode);
        }
}
void CursesProvider::markItemRead(ITEM* item){
        if(item_opts(item)){
                item_opts_off(item, O_SELECTABLE);
                updateStatusLine("[Marking post read]", "", true);
                refresh();

                std::string errorMessage;
                try{
                        const auto& postData = feedly.getSinglePostData(item_index(item));
                        feedly.markPostsRead({postData.id});
                        numUnread--;
                }
                catch (const std::exception& e){
                        errorMessage = e.what();
                }

                updateStatusLine(errorMessage, "", errorMessage.empty());
                update_panels();
        }
}
// Mark an article as read if it has been shown for more than a certain period of time.
void CursesProvider::markItemReadAutomatically(ITEM* item){
        const auto now = std::chrono::steady_clock::now();
        if ((item != NULL) &&
            (now > lastPostSelectionTime) &&
            (secondsToMarkAsRead >= std::chrono::seconds::zero()) &&
            ((now - lastPostSelectionTime) > secondsToMarkAsRead)){
                markItemRead(item);
        }

        lastPostSelectionTime = now;
}
void CursesProvider::renderWindow(WINDOW *win, const char *label, bool highlight){
        const auto width = getmaxx(win);
        mvwaddch(win, 2, 0, ACS_LTEE);
        mvwhline(win, 2, 1, ACS_HLINE, width - 2);
        mvwaddch(win, 2, width - 1, ACS_RTEE);

        const auto labelColor = highlight ? 1 : 2;
        wattron(win, COLOR_PAIR(labelColor));
        box(win, 0, 0);
        mvwaddch(win, 2, 0, ACS_LTEE);
        mvwhline(win, 2, 1, ACS_HLINE, width - 2);
        mvwaddch(win, 2, width - 1, ACS_RTEE);
        printInMiddle(win, 1, 0, width, label, COLOR_PAIR(labelColor));
        wattroff(win, COLOR_PAIR(labelColor));
}
void CursesProvider::print(WINDOW* window, const std::string& s, size_t x, size_t n, short color){
        if(!s.empty()){
                const auto width = getmaxx(window);
                throwIfError(width, "get the window width");
                if(x < static_cast<size_t>(width)){
                        if(color != 0){
                                throwIfError(wattron(statusWin, COLOR_PAIR(color)), "set the color pair");
                        }

                        n = std::min(static_cast<size_t>(width - x), n);
                        throwIfError(mvwprintw(window, 0, x, s.substr(0, n).c_str()), "print a message on a window");

                        if(color != 0){
                                throwIfError(wattroff(statusWin, COLOR_PAIR(color)), "set the color pair");
                        }
                }
        }
}
void CursesProvider::printInMiddle(WINDOW *win, int starty, int startx, int width, const char *str, chtype color){
        int length, x, y;
        float temp;

        if(win == NULL)
                win = stdscr;
        getyx(win, y, x);
        if(startx != 0)
                x = startx;
        if(starty != 0)
                y = starty;
        if(width == 0)
                width = 80;

        length = strlen(str);
        temp = (width - length)/ 2;
        x = startx + (int)temp;
        mvwprintw(win, y, x, "%s", str);
}
void CursesProvider::printPostMenuMessage(const std::string& message){
        const auto height = getmaxy(postsMenuWin);
        const auto width = getmaxx(postsMenuWin);
        const auto y = height / 2;
        const auto x = (width - message.length()) / 2;

        werase(postsMenuWin);
        wattron(postsMenuWin, 1);
        mvwprintw(postsMenuWin, y, x, message.c_str());
        wattroff(postsMenuWin, 1);
}
void CursesProvider::clearStatusLine(){
        throwIfError(werase(statusWin), "clear the status line");
}
void CursesProvider::updateStatusLine(std::string statusMessage, std::string postTitle, bool showCounter){
        clearStatusLine();

        auto counter = std::string{};
        if(showCounter){
                const auto numRead = totalPosts - numUnread;
                counter = "[" + std::to_string(numUnread) + ":" + std::to_string(numRead) + "/" + std::to_string(totalPosts);
        }

        if(!statusMessage.empty()){
                const auto n = u(COLS - counter.length() - 1);
                throwIfError(wattron(statusWin, COLOR_PAIR(1)), "set the color pair 1");
                throwIfError(mvwprintw(statusWin, 0, 0, statusMessage.substr(0, n).c_str()), "print the status message");
                throwIfError(wattroff(statusWin, COLOR_PAIR(1)), "unset the color pair 1");
        }

        if(!postTitle.empty()){
                const auto x = statusMessage.empty() ? 0 : (statusMessage.length() + 1);
                const auto n = u(COLS - x - counter.length() - 1);
                throwIfError(mvwprintw(statusWin, 0, x, postTitle.substr(0, n).c_str()), "print the post title");
        }

        if(!counter.empty()){
                if(u(COLS) > counter.length()){
                        const auto x = u(COLS - counter.length() - 1);
                        throwIfError(wattron(statusWin, COLOR_PAIR(3)), "set the color pair 3");
                        throwIfError(mvwprintw(statusWin, 0, x, counter.c_str()), "print the counter");
                        throwIfError(wattroff(statusWin, COLOR_PAIR(3)), "unset the color pair 3");
                }
        }

        throwIfError(refresh(), "refresh the screen");
        update_panels();
}
void CursesProvider::updateInfoLine(const std::string& info){
        infoMessage = info;
        refreshInfoLine();
}
void CursesProvider::refreshInfoLine(){
        throwIfError(werase(infoWin), "clear the info message");
        if(const auto n = u(infoWinSize.width - 1); (n * infoWinSize.height) > 0){
                throwIfError(wattron(infoWin, COLOR_PAIR(5)), "turn on an attribute");
                throwIfError(mvwprintw(infoWin, 0, 0, infoMessage.substr(0, n).c_str()), "print the info message");
                throwIfError(wattroff(infoWin, COLOR_PAIR(5)), "turn off an attribute");
        }
}
int CursesProvider::execute(const std::string& command, const std::string& arg){
        throwIfError(def_prog_mode(), "make the program mode default");
        throwIfError(endwin(), "end the curses window");

        switch(fork()){
        case -1:{
                throw std::runtime_error("Failed to fork");
        }
        case 0:{
                // Suppress the standard output from a web browser in a GUI session
                // (detected based on DISPLAY and WAYLAND_DISPLAY environment variables)
                // because such a browser may output error messages to the standard output.
                if(getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")){
                        if (close(STDOUT_FILENO) == -1){
                                std::cerr << "Warning: Feednix failed to close STDOUT (" << strerror(errno) << ")" << std::endl;
                        }
                }

                if(close(STDERR_FILENO) == -1){
                        std::cerr << "Warning: Feednix failed to close STDERR (" << strerror(errno) << ")" << std::endl;
                }

                execlp(command.c_str(), command.c_str(), arg.c_str(), NULL);
                std::cerr << "Failed to execute '" << command << "' '" << arg << "'" << std::endl;
                std::cerr << strerror(errno) << std::endl;
                exit(errno);
        }
        default:{
                int wstatus;
                wait(&wstatus);
                throwIfError(reset_prog_mode(), "update the screen");
                return WEXITSTATUS(wstatus);
        }
        }
}
void CursesProvider::clearCategoryItems(){
        for(const auto& ctgItem : ctgItems){
                if(ctgItem != NULL){
                        free_item(ctgItem);
                }
        }

        ctgItems.clear();
}
void CursesProvider::clearPostItems(){
        for(const auto& postItem : postsItems){
                if(postItem != NULL){
                        free_item(postItem);
                }
        }

        postsItems.clear();
}
CursesProvider::~CursesProvider(){
        for(const auto& panel : {ctgPanel, postsPanel, viewPanel, statusPanel, infoPanel}){
                if(panel != NULL){
                        del_panel(panel);
                }
        }

        for(const auto& menu : {ctgMenu, postsMenu}){
                if(menu != NULL){
                        unpost_menu(menu);
                        free_menu(menu);
                }
        }

        clearCategoryItems();
        clearPostItems();
        endwin();
        feedly.curl_cleanup();
}
