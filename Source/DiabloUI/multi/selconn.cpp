#include <fmt/format.h>

#include "DiabloUI/diabloui.h"
#include "DiabloUI/selok.h"
#include "net/realm_client.h"
#include "options.h"
#include "stores.h"
#include "storm/storm_net.hpp"
#include "utils/language.h"
#include "utils/utf8.hpp"

namespace devilution {

int provider;
const char *ConnectionNames[] {
	"ZeroTier",
	N_("Client-Server (TCP)"),
	N_("Offline"),
	N_("Closed Realm (DX:Eternity)"),
};

namespace {

char selconn_MaxPlayers[64];
char selconn_Description[256];
char selconn_Gateway[129];
bool selconn_ReturnValue = false;
bool selconn_EndMenu = false;
GameData *selconn_GameData;

std::vector<std::unique_ptr<UiListItem>> vecConnItems;
std::vector<std::unique_ptr<UiItemBase>> vecSelConnDlg;

#define DESCRIPTION_WIDTH 205

void SelconnEsc();
void SelconnFocus(int value);
void SelconnSelect(int value);

bool RunTextPromptDialog(const char *heading, const char *prompt, const char *descText, char *buffer, std::size_t maxLen, bool allowEmpty)
{
	std::vector<std::unique_ptr<UiItemBase>> dlg;
	bool accepted = false;
	bool done = false;

	LoadBackgroundArt("ui_art\\selgame");
	UiAddBackground(&dlg);
	UiAddLogo(&dlg);

	const Point uiPosition = GetUIRectangle().position;

	char wrappedDesc[256];
	CopyUtf8(wrappedDesc, WordWrapString(descText, DESCRIPTION_WIDTH), sizeof(wrappedDesc));

	SDL_Rect rectTitle = { (Sint16)(uiPosition.x + 24), (Sint16)(uiPosition.y + 161), 590, 35 };
	dlg.push_back(std::make_unique<UiArtText>(heading, rectTitle, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	SDL_Rect rectDescLabel = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 211), 205, 192 };
	dlg.push_back(std::make_unique<UiArtText>(_("Realm Info:").data(), rectDescLabel, UiFlags::FontSize24 | UiFlags::ColorUiSilver));

	SDL_Rect rectDesc = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 256), DESCRIPTION_WIDTH, 192 };
	dlg.push_back(std::make_unique<UiArtText>(wrappedDesc, rectDesc, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark, 1, 16));

	SDL_Rect rectPrompt = { (Sint16)(uiPosition.x + 305), (Sint16)(uiPosition.y + 211), 285, 33 };
	dlg.push_back(std::make_unique<UiArtText>(prompt, rectPrompt, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	SDL_Rect rectEdit = { (Sint16)(uiPosition.x + 305), (Sint16)(uiPosition.y + 314), 285, 33 };
	dlg.push_back(std::make_unique<UiEdit>(prompt, buffer, maxLen, allowEmpty, rectEdit, UiFlags::FontSize24 | UiFlags::ColorUiGold));

	SDL_Rect rectOk = { (Sint16)(uiPosition.x + 299), (Sint16)(uiPosition.y + 427), 140, 35 };
	dlg.push_back(std::make_unique<UiArtTextButton>(_("OK"), &UiFocusNavigationSelect, rectOk, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	SDL_Rect rectCancel = { (Sint16)(uiPosition.x + 449), (Sint16)(uiPosition.y + 427), 140, 35 };
	dlg.push_back(std::make_unique<UiArtTextButton>(_("CANCEL"), &UiFocusNavigationEsc, rectCancel, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	static bool *sAccepted = nullptr;
	static bool *sDone = nullptr;
	sAccepted = &accepted;
	sDone = &done;

	UiInitList(
	    nullptr,
	    [](int) {
		    *sAccepted = true;
		    *sDone = true;
	    },
	    []() {
		    *sAccepted = false;
		    *sDone = true;
	    },
	    dlg);

	while (!done) {
		UiClearScreen();
		UiPollAndRender();
	}
	UiInitList_clear();
	ArtBackground = std::nullopt;
	return accepted;
}

bool RunClosedRealmAccountDialog()
{
	while (true) {
		std::vector<std::unique_ptr<UiListItem>> items;
		std::vector<std::unique_ptr<UiItemBase>> dlg;
		int chosenAction = -1;
		bool menuDone = false;

		LoadBackgroundArt("ui_art\\selgame");
		UiAddBackground(&dlg);
		UiAddLogo(&dlg);

		const Point uiPosition = GetUIRectangle().position;

		char infoBuf[256];
		std::snprintf(
		    infoBuf,
		    sizeof(infoBuf),
		    "Server:\n%s:%u\n\nLog in with your Closed Realm account or register a new account.",
		    sgOptions.Network.szRealmHost[0] != '\0' ? sgOptions.Network.szRealmHost : "127.0.0.1",
		    static_cast<unsigned>(*sgOptions.Network.realmPort != 0 ? *sgOptions.Network.realmPort : 6112));
		CopyUtf8(infoBuf, WordWrapString(infoBuf, DESCRIPTION_WIDTH), sizeof(infoBuf));

		SDL_Rect rectTitle = { (Sint16)(uiPosition.x + 24), (Sint16)(uiPosition.y + 161), 590, 35 };
		dlg.push_back(std::make_unique<UiArtText>(_("Closed Realm (DX:Eternity)").data(), rectTitle, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

		SDL_Rect rectDescLabel = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 211), 205, 192 };
		dlg.push_back(std::make_unique<UiArtText>(_("Realm Status:").data(), rectDescLabel, UiFlags::FontSize24 | UiFlags::ColorUiSilver));

		SDL_Rect rectDesc = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 256), DESCRIPTION_WIDTH, 192 };
		dlg.push_back(std::make_unique<UiArtText>(infoBuf, rectDesc, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark, 1, 16));

		SDL_Rect rectHeader = { (Sint16)(uiPosition.x + 300), (Sint16)(uiPosition.y + 211), 295, 33 };
		dlg.push_back(std::make_unique<UiArtText>(_("Realm Account").data(), rectHeader, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

		items.push_back(std::make_unique<UiListItem>(_("Log In"), 0, UiFlags::ColorUiGold));
		items.push_back(std::make_unique<UiListItem>(_("Create Account"), 1, UiFlags::ColorUiGold));
		items.push_back(std::make_unique<UiListItem>(_("Change Server IP"), 2, UiFlags::ColorUiGold));

		dlg.push_back(std::make_unique<UiList>(items, items.size(), uiPosition.x + 305, (uiPosition.y + 265), 285, 26, UiFlags::AlignCenter | UiFlags::FontSize24));

		SDL_Rect rectOk = { (Sint16)(uiPosition.x + 299), (Sint16)(uiPosition.y + 427), 140, 35 };
		dlg.push_back(std::make_unique<UiArtTextButton>(_("OK"), &UiFocusNavigationSelect, rectOk, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

		SDL_Rect rectCancel = { (Sint16)(uiPosition.x + 449), (Sint16)(uiPosition.y + 427), 140, 35 };
		dlg.push_back(std::make_unique<UiArtTextButton>(_("CANCEL"), &UiFocusNavigationEsc, rectCancel, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

		static int *sChosen = nullptr;
		static bool *sMenuDone = nullptr;
		sChosen = &chosenAction;
		sMenuDone = &menuDone;

		UiInitList(
		    nullptr,
		    [](int idx) {
			    *sChosen = idx;
			    *sMenuDone = true;
		    },
		    []() {
			    *sChosen = -1;
			    *sMenuDone = true;
		    },
		    dlg,
		    true);

		while (!menuDone) {
			UiClearScreen();
			UiPollAndRender();
		}
		UiInitList_clear();
		ArtBackground = std::nullopt;

		if (chosenAction < 0)
			return false;

		if (chosenAction == 2) {
			char hostBuf[129] {};
			CopyUtf8(hostBuf, sgOptions.Network.szRealmHost[0] != '\0' ? sgOptions.Network.szRealmHost : "127.0.0.1", sizeof(hostBuf));
			if (RunTextPromptDialog(
			        _("Closed Realm Server").data(),
			        _("Enter Server IP").data(),
			        _("Enter the IPv4 address or hostname of the DX:Eternity Closed Realm server (also saved in diablo.ini under [Network] Realm Host).").data(),
			        hostBuf,
			        128,
			        false)) {
				CopyUtf8(sgOptions.Network.szRealmHost, hostBuf, sizeof(sgOptions.Network.szRealmHost));
				GetRealmClient().SetServerEndpoint(sgOptions.Network.szRealmHost, *sgOptions.Network.realmPort);
				SaveOptions();
			}
			continue;
		}

		const bool isRegister = (chosenAction == 1);
		char userBuf[32] {};
		CopyUtf8(userBuf, sgOptions.Network.szRealmUsername, sizeof(userBuf));
		if (!RunTextPromptDialog(
		        isRegister ? _("Create Realm Account").data() : _("Realm Account Login").data(),
		        _("Enter Username").data(),
		        _("Account usernames must be 3 to 20 characters (letters, numbers, '_', or '-').").data(),
		        userBuf,
		        20,
		        false)) {
			continue;
		}

		char passBuf[32] {};
		if (!RunTextPromptDialog(
		        isRegister ? _("Create Realm Account").data() : _("Realm Account Login").data(),
		        _("Enter Password").data(),
		        _("Enter your Closed Realm account password.").data(),
		        passBuf,
		        24,
		        false)) {
			continue;
		}

		GetRealmClient().SetServerEndpoint(
		    sgOptions.Network.szRealmHost[0] != '\0' ? sgOptions.Network.szRealmHost : "127.0.0.1",
		    *sgOptions.Network.realmPort != 0 ? *sgOptions.Network.realmPort : 6112);

		std::string errMsg;
		const bool ok = isRegister
		    ? GetRealmClient().RegisterAccount(userBuf, passBuf, errMsg)
		    : GetRealmClient().LoginAccount(userBuf, passBuf, errMsg);

		if (ok) {
			CopyUtf8(sgOptions.Network.szRealmUsername, userBuf, sizeof(sgOptions.Network.szRealmUsername));
			SaveOptions();
			return true;
		}

		if (errMsg.empty())
			errMsg = "Authentication failed.";
		UiSelOkDialog(_("Closed Realm (DX:Eternity)").data(), errMsg.c_str(), false);
	}
}

void SelconnLoad()
{
	LoadBackgroundArt("ui_art\\selconn");

#ifndef NONET
	vecConnItems.push_back(std::make_unique<UiListItem>(_(ConnectionNames[SELCONN_REALM]), SELCONN_REALM));
#ifndef DISABLE_ZERO_TIER
	vecConnItems.push_back(std::make_unique<UiListItem>(ConnectionNames[SELCONN_ZT], SELCONN_ZT));
#endif
#ifndef DISABLE_TCP
	vecConnItems.push_back(std::make_unique<UiListItem>(_(ConnectionNames[SELCONN_TCP]), SELCONN_TCP));
#endif
#endif
	vecConnItems.push_back(std::make_unique<UiListItem>(_(ConnectionNames[SELCONN_LOOPBACK]), SELCONN_LOOPBACK));

	UiAddBackground(&vecSelConnDlg);
	UiAddLogo(&vecSelConnDlg);

	const Point uiPosition = GetUIRectangle().position;

	SDL_Rect rect1 = { (Sint16)(uiPosition.x + 24), (Sint16)(Sint16)(uiPosition.y + 161), 590, 35 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(_("Multi Player Game").data(), rect1, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	SDL_Rect rect2 = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 218), DESCRIPTION_WIDTH, 21 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(selconn_MaxPlayers, rect2, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

	SDL_Rect rect3 = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 256), DESCRIPTION_WIDTH, 21 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(_("Requirements:").data(), rect3, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

	SDL_Rect rect4 = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 275), DESCRIPTION_WIDTH, 66 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(selconn_Description, rect4, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark, 1, 16));

	SDL_Rect rect5 = { (Sint16)(uiPosition.x + 30), (Sint16)(uiPosition.y + 356), 220, 31 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(_("Realm Gateway").data(), rect5, UiFlags::AlignCenter | UiFlags::FontSize24 | UiFlags::ColorUiSilver, 0));

	SDL_Rect rect6 = { (Sint16)(uiPosition.x + 35), (Sint16)(uiPosition.y + 393), DESCRIPTION_WIDTH, 21 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(selconn_Gateway, rect6, UiFlags::AlignCenter | UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

	SDL_Rect rect7 = { (Sint16)(uiPosition.x + 300), (Sint16)(uiPosition.y + 211), 295, 33 };
	vecSelConnDlg.push_back(std::make_unique<UiArtText>(_("Select Connection").data(), rect7, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	SDL_Rect rect8 = { (Sint16)(uiPosition.x + 16), (Sint16)(uiPosition.y + 427), 250, 35 };
	vecSelConnDlg.push_back(std::make_unique<UiArtTextButton>(_("Change Gateway"), nullptr, rect8, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold | UiFlags::ElementHidden));

	vecSelConnDlg.push_back(std::make_unique<UiList>(vecConnItems, vecConnItems.size(), uiPosition.x + 305, (uiPosition.y + 256), 285, 26, UiFlags::AlignCenter | UiFlags::FontSize12 | UiFlags::VerticalCenter | UiFlags::ColorUiGoldDark));

	SDL_Rect rect9 = { (Sint16)(uiPosition.x + 299), (Sint16)(uiPosition.y + 427), 140, 35 };
	vecSelConnDlg.push_back(std::make_unique<UiArtTextButton>(_("OK"), &UiFocusNavigationSelect, rect9, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	SDL_Rect rect10 = { (Sint16)(uiPosition.x + 454), (Sint16)(uiPosition.y + 427), 144, 35 };
	vecSelConnDlg.push_back(std::make_unique<UiArtTextButton>(_("Cancel"), &UiFocusNavigationEsc, rect10, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	UiInitList(SelconnFocus, SelconnSelect, SelconnEsc, vecSelConnDlg, true);
}

void SelconnFree()
{
	ArtBackground = std::nullopt;

	vecConnItems.clear();

	vecSelConnDlg.clear();
}

void SelconnEsc()
{
	gbIsClosedRealm = false;
	selconn_ReturnValue = false;
	selconn_EndMenu = true;
}

void SelconnFocus(int value)
{
	int players = MAX_PLRS;
	std::snprintf(selconn_Gateway, sizeof(selconn_Gateway), "%s", _("no gateway needed").data());
	switch (vecConnItems[value]->m_value) {
	case SELCONN_REALM:
		CopyUtf8(selconn_Description, _("Server-authoritative Closed Realm with online account & character vault."), sizeof(selconn_Description));
		std::snprintf(
		    selconn_Gateway,
		    sizeof(selconn_Gateway),
		    "%s:%u",
		    sgOptions.Network.szRealmHost[0] != '\0' ? sgOptions.Network.szRealmHost : "127.0.0.1",
		    static_cast<unsigned>(*sgOptions.Network.realmPort != 0 ? *sgOptions.Network.realmPort : 6112));
		players = MAX_PLRS;
		break;
	case SELCONN_TCP:
		CopyUtf8(selconn_Description, _("All computers must be connected to a TCP-compatible network."), sizeof(selconn_Description));
		players = MAX_PLRS;
		break;
	case SELCONN_ZT:
		CopyUtf8(selconn_Description, _("All computers must be connected to the internet."), sizeof(selconn_Description));
		players = MAX_PLRS;
		break;
	case SELCONN_LOOPBACK:
		CopyUtf8(selconn_Description, _("Play by yourself with no network exposure."), sizeof(selconn_Description));
		players = 1;
		break;
	}

	CopyUtf8(selconn_MaxPlayers, fmt::format(fmt::runtime(_("Players Supported: {:d}")), players), sizeof(selconn_MaxPlayers));
	CopyUtf8(selconn_Description, WordWrapString(selconn_Description, DESCRIPTION_WIDTH), sizeof(selconn_Description));
}

void SelconnSelect(int value)
{
	provider = vecConnItems[value]->m_value;

	SelconnFree();
	if (provider == SELCONN_REALM) {
		if (!RunClosedRealmAccountDialog()) {
			gbIsClosedRealm = false;
			SelconnLoad();
			return;
		}
		gbIsClosedRealm = true;
	} else {
		gbIsClosedRealm = false;
	}
	selconn_EndMenu = SNetInitializeProvider(provider, selconn_GameData);
	SelconnLoad();
}

} // namespace

bool UiSelectProvider(GameData *gameData)
{
	selconn_GameData = gameData;
	SelconnLoad();

	selconn_ReturnValue = true;
	selconn_EndMenu = false;
	while (!selconn_EndMenu) {
		UiClearScreen();
		UiPollAndRender();
	}
	SelconnFree();

	return selconn_ReturnValue;
}

} // namespace devilution
