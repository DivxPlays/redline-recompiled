# Xbox Live in Redline Recompiled — research report (0.2)

## What the game has

Forza Motorsport's online side is large. The game contains 69 online classes:

- **Live main menu and sign-in:** CUILiveMainMenu, CUILiveLogin.
- **Friends and players:** CUILiveFriends, CUILivePlayers.
- **Car Clubs:** CUICarClubMain, invites, members, logos.
- **Lobbies and sessions:** CUILobby, team colours, trade requests.
- **Online career events:** CUIOnlineCareerEventSelectWindow.
- **Scoreboards:** CUIScoreboardsMain, Worlds Best, Career ELO, driver challenges, free-run times.
- **Voice chat:** CNetVoice.

All of it runs on two Xbox libraries built into the game's own code: **XNet**, the Xbox's network stack, and
**XOnline**, Xbox Live's client: sign-in (Kerberos), matchmaking, stats, friends.

Today the port gives the game a network card that never reports a cable (main.c: "PhyGetLinkState reports no link").
So:
- System Link is greyed out.
- Xbox Live can only ask to quit to the dashboard, which this port can't do.
- Scoreboards are greyed out.

## The revival: Insignia

[Insignia](https://insignia.live) is a fan-run replacement for original Xbox Live. It **lists Forza Motorsport**
under its title ID 4D53006E (the same as your copy) with about 300 scoreboards, so people do play it there. At the
time of writing its page shows no public matches and says it has no details yet for this game. Insignia works with
real Xboxes and with the xemu emulator: you run Insignia's setup once, which creates an Xbox Live account for that
console.

## Three ways forward

### 1. Browsable screens, offline (small to medium)

Fake a successful sign-in so the Live menus open. You could walk through Car Clubs, Friends and Scoreboards, but
every one would be empty or show an error, because there's no server. **Little value on its own**, and the fakes
would be thrown away later.

### 2. A working network card: System Link between PCs (large)

Emulate the Xbox's network card (nForce Ethernet) and connect it to your PC's network, the way xemu does. The game's
own XNet stack then runs unchanged on top. This would give:

- **System Link**: two PCs running Redline Recompiled race each other over a LAN. Over the internet it needs a
  tunnel such as XLink Kai.
- The foundation for option 3.

Work: card emulation (registers, buffers, interrupts) plus a way to pass packets to Windows. That means either a UDP
tunnel with no extra install, or packet capture with Npcap, which players would have to install.

### 3. Insignia: real online play (very large, uncertain)

Needs option 2, plus:

- **Real cryptography:** Xbox Live sign-in uses DES-family Kerberos crypto, and the port currently passes DES
  through without encrypting (a known shortcut).
- **A provisioned console:** the Xbox's identity (EEPROM serial and keys) and an Insignia account. Insignia's setup
  only runs on an Xbox or in xemu, so the likely route is to set up in xemu, then copy the account and console data
  into the port's `hdd` folder.
- **Matching a real Xbox closely** enough that Insignia's servers accept it.

Whether Insignia accepts a recompiled client at all can't be known without trying. It's also worth asking the
Insignia team before connecting, since it's their service.

## Recommendation

Skip option 1. If online play matters, do **option 2 first**: it delivers System Link on its own and is the
unavoidable base for Insignia. Decide on option 3 after that, ideally once the Insignia team has been asked.
