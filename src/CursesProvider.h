#include <chrono>
#include <iostream>

#include <curses.h>
#include <menu.h>
#include <panel.h>

#ifndef _CURSES_H
#define _CURSES_H

#include "FeedlyProvider.h"

constexpr auto CTG_WIN_WIDTH = 40U;
constexpr auto VIEW_WIN_HEIGHT_PER = 50U;

typedef struct{
        unsigned int x;
        unsigned int y;
        unsigned int width;
        unsigned int height;
} WindowSize;

class CursesProvider{
        public:
                CursesProvider(const std::filesystem::path& tmpPath, bool verbose, bool change);
                void init();
                void control();
                ~CursesProvider();
        private:
                FeedlyProvider feedly;
                WINDOW *ctgWin;
                WINDOW *postsWin;
                WINDOW *viewWin;
                WINDOW *ctgMenuWin;
                WINDOW *postsMenuWin;
                WINDOW *statusWin;
                WINDOW *infoWin;
                PANEL* ctgPanel;
                PANEL* postsPanel;
                PANEL* viewPanel;
                PANEL* statusPanel;
                PANEL* infoPanel;
                std::vector<ITEM*> ctgItems{};
                std::vector<ITEM*> postsItems{};
                MENU *ctgMenu, *postsMenu;
                std::string lastEntryRead;
                std::chrono::time_point<std::chrono::steady_clock> lastPostSelectionTime{std::chrono::time_point<std::chrono::steady_clock>::max()};
                std::chrono::seconds secondsToMarkAsRead;
                std::string textBrowser;
                std::string infoMessage;
                const std::filesystem::path previewPath;
                bool currentRank{};
                unsigned int totalPosts{};
                unsigned int numUnread{};
                int viewWinHeightPer = VIEW_WIN_HEIGHT_PER;
                int viewWinHeight = 0;
                int ctgWinWidth = CTG_WIN_WIDTH;
                WindowSize ctgWinSize{};
                WindowSize ctgMenuWinSize{};
                WindowSize postsWinSize{};
                WindowSize postsMenuWinSize{};
                WindowSize viewWinSize{};
                WindowSize statusWinSize{};
                WindowSize infoWinSize{};
                void calculateWindowSizes();
                void clearCategoryItems();
                void clearPostItems();
                void createCategoriesMenu();
                void createPostsMenu();
                void changeSelectedItem(MENU* curMenu, int req);
                void ctgMenuCallback(const char* label);
                void postsMenuCallback(ITEM* item, bool preview);
                void markItemRead(ITEM* item);
                void markItemReadAutomatically(ITEM* item);
                void renderWindow(WINDOW *win, const char *label, bool highlight);
                void printInMiddle(WINDOW *win, int starty, int startx, int width, const char *string, chtype color);
                void printPostMenuMessage(const std::string& message);
                void clearStatusLine();
                void print(WINDOW* window, const std::string& s, size_t x, size_t n, short color);
                void updateStatusLine(std::string statusMessage, std::string postTitle, bool showCounter);
                void updateInfoLine(const std::string& info);
                void refreshInfoLine();
                int execute(const std::string& command, const std::string& arg);
};

#endif
