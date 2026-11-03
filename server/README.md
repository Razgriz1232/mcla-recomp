# The online server: website, accounts and matchmaking

`mcla-server.js` is one Node.js app that is three things for the game's online patch (`mcla_online`; the player's side is in [docs/online.md](../docs/online.md)):

- **The website.** Players create an account there (a gamertag, a password, an email if they like) and get a **game key**. The account page shows the lines to put in `mcla.toml`: the key, and the server's address when the game does not already point at this server. The home page lists who is in the game and which lobbies are open.
- **The account system.** The game signs in with the key (`Authorization: Bearer <key>` on every request). The account's gamertag and online XUID become the player's identity in the game, so two PCs can never be the same player and a name belongs to whoever registered it first.
- **The matchmaking server.** It keeps who is online and which sessions (lobbies) exist, answers the game's lobby searches and hands out the QoS data hosts publish. The races themselves run between the players' PCs over UDP and never pass through the server.
- **Levels.** Every race finished online pays XP, a win more, and the XP makes a level and a title that show next to the player on the site, on their account page and on the leaderboard (`/leaderboard`). See [Levels and XP](#levels-and-xp).

Plain Node.js 16 or newer. Everything it keeps is in JSON files in its `data` folder (accounts, who is online, the lobbies, the rides, a secret for the login cookies), so a restart loses nothing; or, with a MySQL database configured, in that database, so a re-upload of the app loses nothing either (see [MySQL](#mysql-keeping-the-accounts-outside-the-app-folder)). The one package, `mysql2`, is only loaded when a database is configured.

## Running it yourself

```bash
node server/mcla-server.js
```

Options: `--port 36000` (the default), `--bind 0.0.0.0` (every interface, the default; `127.0.0.1` for this PC only), `--data ./data` (the folder for the JSON files; by default `data` next to the script), `--base-path /mcla` (when the app lives under a path of a site), `--patch-url URL` (the download link on the home page), `--verbose` (every request), `--trust-proxy` (take the client's address from `X-Forwarded-For`), `--xp-finish 50` and `--xp-win 150` (the XP a finished race and a win pay), `--win-property 10000010` and `--win-value 1` (the stats property that marks a win; see [Levels and XP](#levels-and-xp)), `--game-default-server https://mclaonline.com` (the server the game points at when `mcla_online_server` is not set: when this site is it, the account page leaves that line out). The environment does the same: `PORT`, `MCLA_SERVER_BIND`, `MCLA_DATA_DIR`, `MCLA_DB_URL` (or `MCLA_DB_HOST`, `MCLA_DB_PORT`, `MCLA_DB_USER`, `MCLA_DB_PASSWORD`, `MCLA_DB_NAME`: the MySQL database that holds everything instead of the data folder; `MCLA_DB_SSL` = `1` for TLS to it, `insecure` to skip the certificate check), `PASSENGER_BASE_URI`, `MCLA_PATCH_URL`, `MCLA_SITE_NAME`, `MCLA_VERBOSE`, `MCLA_XP_FINISH`, `MCLA_XP_WIN`, `MCLA_WIN_PROPERTY`, `MCLA_WIN_VALUE`, `MCLA_GAME_DEFAULT_SERVER`, and `MCLA_COOKIE_SECRET` (the secret that signs the login cookies; made on first start and kept in `data/secret.json` when unset).

Open `http://localhost:36000/` in a browser for the site. The background photo is `assets/la-sunset.webp`, served at `/assets/la-sunset.webp`; replace the file to change it (any `.webp`, `.jpg` or `.png` in `assets` is served by its plain name).

## MySQL: keeping the accounts outside the app folder

Without a database the accounts live in `data/accounts.json` next to the server file, and replacing the app's folder at an update takes them with it. With a database the four stores (accounts, state, secret, rides) are rows of a table `mcla_store`, and the rides' car data and photos rows of `mcla_files`; the app makes both tables at its first start and fills them from the JSON files when it finds any, so switching keeps what is there. The title storage files (`data/storage/title`) stay files.

On cPanel (GoDaddy):

1. *MySQL Databases*: create a database (cPanel prefixes its name with your user name, `user_mcla`), create a database user with a strong password, and add the user to the database with *All privileges*.
2. *Setup Node.js App*, your app, *Environment variables*: add `MCLA_DB_URL` = `mysql://user_mcla:THE PASSWORD@localhost/user_mcla` (a password with `@`, `/`, `:` or `#` in it must be URL-encoded, `%40` for `@`), or the five `MCLA_DB_HOST`, `MCLA_DB_PORT`, `MCLA_DB_USER`, `MCLA_DB_PASSWORD`, `MCLA_DB_NAME`.
3. *Run NPM Install* (it installs `mysql2`, the one package), then *Restart*. The log (`stderr.log` in the app folder) says `database: ... (mcla_store, mcla_files)` and, the first time, which rows were made from the JSON files.

On GoDaddy's newer *Apps* hosting (an app deployed from a GitHub repository, with Preview and Published copies, not cPanel): its *Database* page makes the database and shows the host, the port, the name and the user to use (the host is not `localhost`); when the database was made from that page, the app's *Secrets* already hold `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER` and `DB_PASSWORD` (set by the platform, not changeable), and the server reads those by itself, so nothing is added; a database made elsewhere goes in as the five `MCLA_DB_*` secrets. TLS is tried by itself for a database that is not on the same machine (`MCLA_DB_SSL` = `1`, `0` or `insecure` decides it otherwise); then *Update Preview* pulls the repository's `main` and installs the package, and *Publish to Live* puts it on the site. The *Runtime Logs* page shows the `database:` line, or why the connection failed. On that hosting a deploy replaces the app's files, which is why the accounts have to be in the database there.

From then on the app's folder holds nothing that matters: upload a new `mcla-server.js` or the whole zip, restart, and the accounts, keys, levels and rides are where they were. Back up the database (cPanel's *Backup*, or phpMyAdmin's export). To go back to files, remove the variables and restart: the JSON files are used again (what was written to the database in between is not copied back).

To try the database mode on a PC without MySQL, `server/dev/standin` has a stand-in for the `mysql2` package that keeps the two tables in a JSON file: `NODE_PATH=server/dev/standin MCLA_DB_URL=mysql://u:p@localhost/test node server/mcla-server.js`.

## Hosting it on shared Node.js hosting (GoDaddy, cPanel)

The app was made for a host that gives you one Node.js app managed by Phusion Passenger, as cPanel's *Setup Node.js App* does on GoDaddy's web hosting:

1. Upload the `server` folder's files (`mcla-server.js`, `app.js`, `package.json` and the `assets` folder with the background photo and the logo) to the application root the host gives you, for example `mcla-online`. Nothing else: no `data` folder (the server makes its own, and yours holds your test accounts) and no `node_modules` (there are none). As a zip (about a third of a megabyte), from the `server` folder: `zip -r ../mcla-online.zip mcla-server.js app.js package.json assets`, or on Windows select the four, right-click, *Send to*, *Compressed (zipped) folder*.
2. In *Setup Node.js App*: application root `mcla-online`, application URL your domain (or a sub-path of it), **application startup file `app.js`**, Node 16 or newer, production mode. Create it and start it.
3. Open the domain: the home page is there, and the account page shows the right address for `mcla_online_server` (it is taken from how the site was reached, `https://yourdomain.com`), or no address at all when the site is the one the game points at by default (`https://mclaonline.com`; `MCLA_GAME_DEFAULT_SERVER` says which).

What the app does for that kind of host, by itself: it listens on the port in `PORT` (the host sets it) on every interface, trusts `X-Forwarded-For` and `X-Forwarded-Proto` from the host's proxy (so players get their real public address, and cookies are marked secure under HTTPS), honours `PASSENGER_BASE_URI` when the app is under a sub-path, and keeps its state in files, re-reading them before each request, so it survives Passenger stopping the app when nobody has used it for a while and starting more than one copy of it. Give the `data` folder a place outside the web-served tree if the host's application root is served as static files (`MCLA_DATA_DIR` in the app's environment variables); `accounts.json` holds password hashes and game keys. Better, use the MySQL database (above): then a re-upload of the app folder cannot lose the accounts.

A platform that deploys a zip and runs `npm install`, `npm run build` and `npm start` itself works the same: `package.json` has the three (`build` has nothing to do), the app binds `PORT`, and the one package, `mysql2`, is only needed with a database. One thing to check on such a platform: whether its disk persists between deploys and restarts. The accounts live in the `data` folder, so point `MCLA_DATA_DIR` at a persistent volume if the app's own folder is replaced on every deploy, and set `MCLA_COOKIE_SECRET` so the login cookies stay valid across restarts.

HTTPS: the game reaches the site with WinHTTP on Windows, so a host's certificate works as it does in a browser, and the address on the account page says `https://...`. If the host forces HTTPS, nothing more is needed. On Linux and the PS5 the game's client is HTTP only (no TLS there yet).

## Reaching each other in the game

The server only lists lobbies. Race traffic goes directly between the players on the game's UDP port (1000; the log says `the game bound UDP port N`):

- **Same LAN**: nothing to do. Players behind one router get each other's LAN addresses.
- **Across the internet**: the player who **hosts** a lobby needs UDP 1000 forwarded on their router to their PC. Joining usually works without it: the joiner's game sends first, which opens the mapping in its router. A VPN that puts everyone on one network (Hamachi, ZeroTier, Tailscale) also does.

## The API

JSON in and out. The game's routes need `Authorization: Bearer <game key>`; without it they answer 401 with the website's address. 64-bit ids (XUIDs, session ids, machine ids, nonces) are 16 hex digits. Addresses are dotted IPv4 text; a player's public address is what the server saw the request come from.

| Method and path | Needs | Body | Does |
|---|---|---|---|
| `GET /` | | | The website. `/register`, `/login`, `/account` (`POST` for the forms and for `/logout`, `/account/key`, `/account/password`), `/leaderboard`. |
| `GET /status` | | | Public counts, who is in the game (names and levels), the open lobbies, the ten top racers, and `storage`: `database` or `files`, where the data lives. |
| `POST /players` | key | `{mac, machineId, lanIp, port, titleId}` | Signs the account's player in. Returns `{xuid, name, publicIp}`: the identity the game takes. |
| `POST /players/:xuid/heartbeat` | key | the same | Keeps the player and the lobbies they host alive. Every 15 s. |
| `GET /players/:xuid` | key | | One player (for arbitration's machine ids). |
| `DELETE /players/:xuid` | key, own | | Signs out; the player's lobbies go. |
| `POST /sessions` | key | `{id, titleId, flags, publicSlots, privateSlots, state, nonce, key, host:{lanIp, port, mac, machineId}, contexts:[{id,value}], properties:[{id,type,data}], members:[{xuid,private}]}` | The account publishes a lobby it hosts. |
| `GET /sessions` | key | | Every lobby, in full. |
| `POST /sessions/search` | key | `{titleId, numResults, contexts:[{id,value}], properties}` | Lobbies of the title with an open public slot, not in a race when the host forbids joining one, whose contexts agree with the search's; the searcher's own left out. Each comes with its `qos`. |
| `GET /sessions/:id` | key | | One lobby, with `qos`. |
| `PUT /sessions/:id` | key, host | any of `flags, publicSlots, privateSlots, state, members, contexts, properties, qos` | The host updates it. |
| `DELETE /sessions/:id` | key, host | | The host deletes it. |
| `POST /sessions/:id/join` | key, host | `{members:[{xuid,private}]}` | Replaces the member list. |
| `POST /sessions/:id/leave` | key, host | `{xuids:[...]}` | Removes members. |
| `POST /sessions/:id/migrate` | key | a lobby like `POST /sessions` | The new host's lobby replaces `:id`. |
| `PUT /sessions/:id/qos` | key, host | `{qos: base64}` | The host's QoS data (XNetQosListen). |
| `GET /sessions/:id/qos` | key | | `{qos: base64}`. |
| `GET /storage/title` | key | `?name=pattern` | The title storage files whose names match (a `*` is a wildcard): `{files:[{name, size, modified}]}`. |
| `GET /storage/title/:name` | key | | One title storage file, as bytes (404 when there is none). |
| `GET /rides` | key | `?by=rank|new|random|gamer&gamer=&xuid=&exclude=&from=&count=` | Rate My Ride: the rides in that order (`gamer`: one player's, by gamertag or by `xuid`; `exclude`: leaving one XUID's out; `count=0`: just the total), `{total, rides:[{id, name, vehicle, owner, ownerXuid, price, rating, ratings, rank, size, fileSizes, uploaded, photo, photo2, userData, category, yours, yourRating}]}`. |
| `POST /rides` | key | `{name, vehicle, price, data: base64, photo: base64, photo2: base64, userData: base64, category}` | Uploads a ride: the car data (up to 2 MB), one or two JPEG or PNG photos (up to 1 MB each), the game's own 64 bytes about it (`userData`: its vehicle, price, times bought and money earned, kept as they are) and its category (0 to 63). Ten a minute per address. |
| `POST /rides/:id` | key, owner | the same | Replaces a ride (the game's "change ride"): new files and data under the same id; its ratings start over. |
| `GET /rides/:id` | key | | One ride. `GET /rides/:id/file` is its car data as bytes; `GET /rides/:id/photo` and `/photo2` its photos (no key: the website shows them). |
| `POST /rides/:id/userdata` | key | `{userData: base64}` | Replaces the game's 64 bytes on any ride (buying a ride credits the seller's record this way). |
| `POST /rides/:id/rate` | key | `{stars: 1..10}` | Rates a ride on the game's scale, 1 to 10 (one rating per account, changed by rating again; not your own). Answers `{rating, ratings, ride}`. |
| `DELETE /rides/:id` | key, owner | | Removes a ride. |
| `POST /stats` | key | `{sessionId, xuid, titleId, views:[{viewId, properties}]}` | A stats write for the player `xuid`, from them or from the host of `sessionId`. Pays the XP of a race finished and, when the win property says so, of a race won, once per player and session; answers `{awards, progress}`. The last 500 writes are kept and logged in full. |

Session `state`: 0 lobby, 1 registration (arbitration done), 2 in game, 3 reporting, 4 deleted. `flags` are the game's `XSESSION_CREATE_*` bits (0x01 host, 0x02 presence, 0x04 stats, 0x08 matchmaking, 0x10 arbitration, 0x20 peer network, 0x400 no joining in progress). Contexts are the game's `XUSER_CONTEXT` ids and values (0x800A game type, 0x800B game mode); the server matches them exactly, except the presence context (0x8001).

## Title storage

Xbox LIVE kept, for each game, files its publisher put there (title-managed storage), and the game reads some at the city load: its post-release content archives (`tms_boot`, `tms_boot2`: string tables, vinyl preset names). The game asks the server for a file by name (`XStorageBuildServerPath`, `XStorageEnumerate`, `XStorageDownloadToMemory`), and the server answers from the `data/storage/title` folder: put a file there and the game downloads it; with no file of that name the game goes on without, as it did before. Rockstar's own archives are not in this repository.

## Rate My Ride

The game's Rate My Ride menu (pause menu, Modes, Xbox Live) let players send a car with its photos to Rockstar's upload service, browse the rides others sent, rate them and buy them; the service is gone. This server has the same thing, and the game's menu uses it (the online layer translates the menu's commands into these routes): rides are kept in `data/rides.json` with the car data and the photos as files in `data/rides`, the game's routes are under `/rides`, and the website shows them under `/ratemyride` (every ride with its photo, best rated or newest first, and a page per ride where signed-in players rate it 1 to 10, as the game does; the home page shows the three best, the account page the player's own). A rating is one per account per ride, changed by rating again; nobody rates their own.

What the game does with it: nothing yet. The game's menu talks Rockstar's own protocol (the RAGE library's upload commands), and the online layer does not translate those into these routes yet; until it does, the menu says the server is not available. The routes are there for that translation, and for anything else that uploads a ride.

## Levels and XP

A race finished online pays 50 XP and a win 150 XP more (`--xp-finish`, `--xp-win`). Level 2 takes 200 XP and each level after it 100 XP more than the one before (level 5 at 1,400, level 10 at 5,400, level 20 at 20,900, up to level 60), with a title by level: Rookie, Street Racer at 5, Pro at 10, Elite at 20, Legend at 35, Midnight Club at 50. The level shows next to the name on the home page, with a progress bar and the last twenty races on the account page, and the leaderboard orders every account that has raced by XP. The game key is the only way to earn XP: an account's XP is in `accounts.json` with the keys of the races already paid, so a race pays each player once however many stats writes it makes.

Where the XP comes from: at the end of a race the game writes its leaderboard stats (`XSessionWriteStats`), which the online layer sends to `POST /stats`, naming the session and the player the stats are for. The server pays the finish XP for a write from the player themself, or from the host of the session for one of its members, as long as the server knows the session and the player is in it; any other write is kept but pays nothing, and the log says why. Which property in that write says who won is the game's own (it is in its leaderboard schema, not in the SDK), so wins are off until it is set: the server logs every write with its properties (`stats by Host for 0009... in session ...: view 0x1: 0x10000010=1 ...`), so one real race shows which property holds the finishing position and what a win reads as; then set `MCLA_WIN_PROPERTY` to that id (hex) and `MCLA_WIN_VALUE` to the value (1 by default), and restart.

## Accounts, in detail

A gamertag is 1 to 15 letters, digits and single spaces, starting with a letter, unique whatever the case. Passwords are 8 characters or more, stored as scrypt hashes with a salt of their own. The game key is 40 hex digits made from random bytes; a player can make a new one at any time on the account page, and the old one stops working at once. The online XUID is derived from the account's number (it begins `0009`, as LIVE's did). Logins stay for 30 days in an HttpOnly cookie signed with the server's secret. The forms accept a few tries a minute per address (5 accounts an hour, 20 logins in 10 minutes).

There is no email sending, so no password reset by mail: a forgotten password means a new account for now, or an edit of `data/accounts.json` by whoever runs the server.
