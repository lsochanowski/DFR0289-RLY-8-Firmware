#include "rly_web.h"

#include <string.h>

#define A(s) (used = rly_buf_append(out, cap, used, (s)))
#define E(s) (used = rly_html_escape(out, cap, used, (s)))
#define U(v) (used = rly_buf_append_uint(out, cap, used, (uint32_t)(v)))

static const char CSS[] =
    "<style>body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#222}"
    "header{background:#1f4e8c;color:#fff;padding:10px 16px}header a{color:#fff;margin-right:14px}"
    "main{max-width:720px;margin:auto;padding:12px 16px}"
    "section{background:#fff;border-radius:8px;padding:12px 16px;margin:12px 0;"
    "box-shadow:0 1px 2px #0002}h2{font-size:1.05em;margin:0 0 8px}"
    "table{border-collapse:collapse;width:100%}td{padding:3px 6px;border-bottom:1px solid #eee}"
    "td:first-child{color:#666;width:40%}label{display:block;margin:6px 0 2px;color:#555}"
    "input,select{width:100%;box-sizing:border-box;padding:6px;font-size:1em}"
    ".r{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}"
    ".r button{padding:12px 4px;font-size:1em;border:0;border-radius:6px;background:#ccd}"
    ".r button.on{background:#2e9d4b;color:#fff}.b{padding:8px 14px;margin:4px 4px 0 0}"
    ".err{background:#fde2e2;color:#900;padding:8px;border-radius:6px}"
    ".ok{background:#e1f5e5;color:#165;padding:8px;border-radius:6px}"
    "@media(prefers-color-scheme:dark){body{background:#16181c;color:#ddd}"
    "section{background:#22252b}td{border-color:#333}.r button{background:#3a3f48;color:#ddd}"
    "input,select{background:#2b2f36;color:#eee;border:1px solid #444}}</style>";

static size_t header(char *out, size_t cap, int code) {
    size_t used = 0;
    if (code == 200) {
        A("HTTP/1.0 200 OK\r\n");
    } else if (code == 400) {
        A("HTTP/1.0 400 Bad Request\r\n");
    } else if (code == 403) {
        A("HTTP/1.0 403 Forbidden\r\n");
    } else {
        A("HTTP/1.0 404 Not Found\r\n");
    }
    A("Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
      "Connection: close\r\n\r\n");
    return used;
}

static size_t page_top(char *out, size_t cap, size_t used, const char *title,
                       const rly_config *cfg) {
    A("<!doctype html><html lang=pl><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
    E(cfg->device_name);
    A(" – ");
    A(title);
    A("</title>");
    A(CSS);
    A("</head><body><header><b>");
    E(cfg->device_name);
    A("</b> &nbsp; <a href=/>Status</a><a href=/settings>Ustawienia</a></header><main>");
    return used;
}

static size_t page_end(char *out, size_t cap, size_t used) {
    A("</main></body></html>");
    return used;
}

static size_t row(char *out, size_t cap, size_t used, const char *k, const char *v) {
    A("<tr><td>");
    A(k);
    A("</td><td>");
    E(v);
    A("</td></tr>");
    return used;
}

static size_t token_field(char *out, size_t cap, size_t used, const char *token) {
    A("<input type=hidden name=t value=");
    A(token);
    A(">");
    return used;
}

static void fmt_mac(const uint8_t *m, char *s) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        s[i * 3] = hex[m[i] >> 4];
        s[i * 3 + 1] = hex[m[i] & 15];
        s[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
}

static void fmt_uptime(uint32_t s, char *buf, size_t cap) {
    size_t used = 0;
    char *out = buf;
    buf[0] = '\0';
    if (s >= 86400u) {
        U(s / 86400u);
        A(" d ");
    }
    U((s / 3600u) % 24u);
    A(" h ");
    U((s / 60u) % 60u);
    A(" min");
}

static size_t page_status(const rly_config *cfg, const rly_web_status *st, const char *token,
                          char *out, size_t cap, const char *notice) {
    size_t used = header(out, cap, 200);
    char tmp[40];
    used = page_top(out, cap, used, "Status", cfg);
    if (notice != NULL) {
        A("<p class=ok>");
        E(notice);
        A("</p>");
    }

    A("<section><h2>Przekaźniki</h2><form method=post action=/relay class=r>");
    used = token_field(out, cap, used, token);
    A("<input type=hidden name=s value=toggle>");
    for (int i = 1; i <= 8; i++) {
        int on = (st->relay_mask >> (i - 1)) & 1;
        A("<button name=n value=");
        U(i);
        A(on ? " class=on>" : ">");
        U(i);
        A(on ? " · ON" : " · OFF");
        A("</button>");
    }
    A("</form><form method=post action=/relay style=margin-top:8px>");
    used = token_field(out, cap, used, token);
    A("<input type=hidden name=n value=all>"
      "<button class=b name=s value=off>Wszystkie OFF</button>"
      "<button class=b name=s value=on>Wszystkie ON</button></form></section>");

    A("<section><h2>Sieć</h2><table>");
    used = row(out, cap, used, "Link",
               st->link_up ? (st->speed_100 ? (st->full_duplex ? "100 Mb/s, full duplex"
                                                               : "100 Mb/s, half duplex")
                                            : (st->full_duplex ? "10 Mb/s, full duplex"
                                                               : "10 Mb/s, half duplex"))
                           : "brak");
    rly_format_ipv4(st->ip, tmp, sizeof tmp);
    used = row(out, cap, used, "Adres IP", tmp);
    rly_format_ipv4(st->mask, tmp, sizeof tmp);
    used = row(out, cap, used, "Maska", tmp);
    rly_format_ipv4(st->gw, tmp, sizeof tmp);
    used = row(out, cap, used, "Brama", tmp);
    used = row(out, cap, used, "Tryb",
               cfg->dhcp ? (st->dhcp_bound ? "DHCP"
                                           : (st->static_fallback ? "DHCP – brak serwera, adres zapasowy"
                                                                  : "DHCP – oczekiwanie"))
                         : "statyczny");
    fmt_mac(st->mac, tmp);
    used = row(out, cap, used, "MAC", tmp);
    used = row(out, cap, used, "Nazwa hosta", st->node_id);
    A("</table></section>");

    A("<section><h2>MQTT / Home Assistant</h2><table>");
    used = row(out, cap, used, "Stan", st->mqtt_state);
    used = row(out, cap, used, "Broker", st->mqtt_broker[0] ? st->mqtt_broker : "–");
    used = row(out, cap, used, "Źródło adresu", st->mqtt_source);
    used = row(out, cap, used, "Prefiks discovery", cfg->mqtt_prefix);
    A("<tr><td>Tematy</td><td>");
    E(st->node_id);
    A("/relay<i>N</i>/set · ");
    E(st->node_id);
    A("/relay<i>N</i>/state</td></tr></table></section>");

    A("<section><h2>Urządzenie</h2><table>");
    fmt_uptime(st->uptime_s, tmp, sizeof tmp);
    used = row(out, cap, used, "Czas pracy", tmp);
    used = row(out, cap, used, "Firmware", st->fw_version);
    used = row(out, cap, used, "Konsola USB", st->usb_connected ? "połączona" : "nieaktywna");
    A("</table><form method=post action=/reboot>");
    used = token_field(out, cap, used, token);
    A("<button class=b>Restart</button></form></section>");
    return page_end(out, cap, used);
}

static size_t text_input(char *out, size_t cap, size_t used, const char *label, const char *name,
                         const char *value, const char *extra) {
    A("<label>");
    A(label);
    A("</label><input name=");
    A(name);
    A(" value=\"");
    E(value);
    A("\"");
    if (extra != NULL) {
        A(" ");
        A(extra);
    }
    A(">");
    return used;
}

static size_t onoff(char *out, size_t cap, size_t used, const char *label, const char *name,
                    int on, const char *on_text, const char *off_text) {
    A("<label>");
    A(label);
    A("</label><select name=");
    A(name);
    A("><option value=on");
    A(on ? " selected>" : ">");
    A(on_text);
    A("</option><option value=off");
    A(on ? ">" : " selected>");
    A(off_text);
    A("</option></select>");
    return used;
}

static size_t page_settings(const rly_config *cfg, const char *token, const char *fw, char *out,
                            size_t cap, int code, const char *error) {
    size_t used = header(out, cap, code);
    char v[RLY_CFG_HOST_MAX];
    used = page_top(out, cap, used, "Ustawienia", cfg);
    if (error != NULL) {
        A("<p class=err>");
        E(error);
        A("</p>");
    }
    A("<form method=post action=/save>");
    used = token_field(out, cap, used, token);

    A("<section><h2>Sieć</h2>");
    used = onoff(out, cap, used, "Adresacja", "dhcp", cfg->dhcp, "DHCP (automatycznie)",
                 "statyczna");
    static const char *const net[][2] = {
        {"ip", "Adres IP (statyczny / zapasowy)"}, {"mask", "Maska"}, {"gw", "Brama"},
        {"dns", "DNS"}};
    for (size_t i = 0; i < 4; i++) {
        rly_config_get(cfg, net[i][0], v, sizeof v, 0);
        used = text_input(out, cap, used, net[i][1], net[i][0], v, "inputmode=decimal");
    }
    used = text_input(out, cap, used, "Nazwa hosta (puste = automatyczna)", "hostname",
                      cfg->hostname, "maxlength=31");
    A("</section>");

    A("<section><h2>MQTT / Home Assistant</h2>");
    used = onoff(out, cap, used, "MQTT", "mqtt", cfg->mqtt_enabled, "włączone", "wyłączone");
    used = text_input(out, cap, used, "Broker (IP lub nazwa; puste = autodetekcja HA)",
                      "mqtt_host", cfg->mqtt_host,
                      "placeholder='autodetekcja (mDNS)' maxlength=63");
    rly_config_get(cfg, "mqtt_port", v, sizeof v, 0);
    used = text_input(out, cap, used, "Port", "mqtt_port", v, "inputmode=numeric");
    used = text_input(out, cap, used, "Użytkownik", "mqtt_user", cfg->mqtt_user,
                      "autocomplete=off maxlength=31");
    used = text_input(out, cap, used, "Hasło", "mqtt_pass", "",
                      cfg->mqtt_pass[0] ? "type=password placeholder='(bez zmian)' "
                                          "autocomplete=new-password maxlength=47"
                                        : "type=password placeholder='(brak)' "
                                          "autocomplete=new-password maxlength=47");
    A("<label><input type=checkbox name=mqtt_pass_clear value=1 style=width:auto> "
      "usuń zapisane hasło MQTT</label>");
    used = text_input(out, cap, used, "Prefiks discovery", "mqtt_prefix", cfg->mqtt_prefix,
                      "maxlength=31");
    used = text_input(out, cap, used, "Nazwa urządzenia w HA", "name", cfg->device_name,
                      "maxlength=31");
    A("</section>");

    A("<section><h2>Hasło panelu (użytkownik admin)</h2>");
    used = text_input(out, cap, used, "Nowe hasło (puste = bez zmian)", "admin_pass", "",
                      "type=password autocomplete=new-password maxlength=47");
    used = text_input(out, cap, used, "Powtórz nowe hasło", "admin_pass2", "",
                      "type=password autocomplete=new-password maxlength=47");
    A("</section><button class=b>Zapisz i uruchom ponownie</button></form>");

    A("<section><h2>Ustawienia fabryczne</h2><p>DHCP, hasło <b>admin</b>, MQTT z "
      "autodetekcją. To samo robi przytrzymanie przycisku na płytce przez 5 s.</p>"
      "<form method=post action=/factory onsubmit=\"return confirm('Przywrócić ustawienia "
      "fabryczne?')\">");
    used = token_field(out, cap, used, token);
    A("<button class=b>Przywróć ustawienia fabryczne</button></form></section>");

    A("<section><h2>Aktualizacja firmware</h2><p>Zainstalowana wersja: <b>");
    E(fw);
    A("</b>. Wybierz plik <b>rly8-fw-v…-update.bin</b>. Ustawienia zostają zachowane.</p>"
      "<input type=file id=fw accept=.bin><progress id=pg max=100 value=0 "
      "style=width:100%;margin-top:8px></progress>"
      "<button class=b type=button onclick=up()>Wgraj i uruchom ponownie</button><p id=us></p>"
      "<script>function up(){var f=document.getElementById('fw').files[0],"
      "s=document.getElementById('us'),g=document.getElementById('pg');"
      "if(!f){s.textContent='Wybierz plik.';return}var x=new XMLHttpRequest();"
      "x.open('POST','/update?t=");
    A(token);
    A("');x.upload.onprogress=function(e){if(e.lengthComputable)g.value=100*e.loaded/e.total};"
      "x.onload=function(){s.textContent=x.responseText;if(x.status==200){"
      "s.textContent+=' Strona odświeży się za 20 s.';setTimeout(function(){location='/'},20000)}};"
      "x.onerror=function(){s.textContent='Błąd połączenia.'};s.textContent='Wysyłanie…';"
      "x.send(f)}</script></section>");
    return page_end(out, cap, used);
}

static size_t page_message(const rly_config *cfg, char *out, size_t cap, const char *msg,
                           const char *next_url) {
    size_t used = header(out, cap, 200);
    used = page_top(out, cap, used, "Restart", cfg);
    A("<section><p class=ok>");
    E(msg);
    A("</p>");
    if (next_url != NULL) {
        A("<p>Za kilka sekund: <a id=n href=\"");
        E(next_url);
        A("\">");
        E(next_url);
        A("</a></p><script>setTimeout(function(){location=document.getElementById('n')"
          ".href},9000)</script>");
    }
    A("</section>");
    return page_end(out, cap, used);
}

static size_t simple(char *out, size_t cap, int code, const char *text) {
    size_t used = header(out, cap, code);
    A(text);
    return used;
}

static size_t redirect_home(char *out, size_t cap) {
    size_t used = 0;
    A("HTTP/1.0 303 See Other\r\nLocation: /\r\nCache-Control: no-store\r\n"
      "Connection: close\r\nContent-Length: 0\r\n\r\n");
    return used;
}

static size_t page_json(const rly_config *cfg, const rly_web_status *st, char *out, size_t cap) {
    size_t used = 0;
    char tmp[40];
    A("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
      "Connection: close\r\n\r\n{\"node\":\"");
    A(st->node_id);
    A("\",\"name\":\"");
    A(cfg->device_name);
    A("\",\"fw\":\"");
    A(st->fw_version);
    A("\",\"ip\":\"");
    rly_format_ipv4(st->ip, tmp, sizeof tmp);
    A(tmp);
    A("\",\"link\":");
    A(st->link_up ? "true" : "false");
    A(",\"uptime\":");
    U(st->uptime_s);
    A(",\"mqtt\":\"");
    A(st->mqtt_state);
    A("\",\"relays\":[");
    for (int i = 0; i < 8; i++) {
        A(((st->relay_mask >> i) & 1) ? "1" : "0");
        if (i < 7) {
            A(",");
        }
    }
    A("]}");
    return used;
}

static size_t unauthorized(char *out, size_t cap) {
    size_t used = 0;
    A("HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"RLY-8\", "
      "charset=\"UTF-8\"\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
      "Login: admin");
    return used;
}

static size_t plain(char *out, size_t cap, int code, const char *text) {
    size_t used = 0;
    A(code == 200 ? "HTTP/1.0 200 OK\r\n"
                  : (code == 403 ? "HTTP/1.0 403 Forbidden\r\n"
                                 : (code == 413 ? "HTTP/1.0 413 Payload Too Large\r\n"
                                                : "HTTP/1.0 400 Bad Request\r\n")));
    A("Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n"
      "Connection: close\r\n\r\n");
    A(text);
    return used;
}

int rly_web_upload_allowed(const rly_http_request *req, const rly_config *cfg, const char *token,
                           char *out, size_t cap, size_t *len) {
    char t[24];
    if (!rly_http_check_basic(req->auth, req->auth_len, "admin", cfg->admin_pass)) {
        *len = unauthorized(out, cap);
        return 0;
    }
    if (token == NULL || token[0] == '\0' ||
        !rly_http_form_get(req->query, strlen(req->query), "t", t, sizeof t) ||
        strcmp(t, token) != 0) {
        *len = plain(out, cap, 403, "Formularz wygasł – odśwież stronę.");
        return 0;
    }
    if (req->content_length < RLY_OTA_HEAD_KEEP) {
        *len = plain(out, cap, 400, "Błąd: plik za mały.");
        return 0;
    }
    if (req->content_length > RLY_SLOT_SIZE) {
        *len = plain(out, cap, 413, "Błąd: plik za duży.");
        return 0;
    }
    return 1;
}

size_t rly_web_upload_result(rly_img_status st, const char *version, char *out, size_t cap) {
    char msg[128];
    size_t m;
    if (st == RLY_IMG_OK) {
        m = rly_buf_append(msg, sizeof msg, 0, "OK: wgrano wersję ");
        m = rly_buf_append(msg, sizeof msg, m, version[0] ? version : "?");
        (void)rly_buf_append(msg, sizeof msg, m, ". Instalacja i restart (ok. 10 s).");
        return plain(out, cap, 200, msg);
    }
    m = rly_buf_append(msg, sizeof msg, 0, "Błąd: ");
    m = rly_buf_append(msg, sizeof msg, m, rly_img_status_text(st));
    (void)rly_buf_append(msg, sizeof msg, m, ". Obecny firmware działa dalej.");
    return plain(out, cap, 400, msg);
}

static int token_ok(const rly_http_request *req, const char *token) {
    char t[24];
    /* An empty token (not generated yet) must never match an empty field. */
    return token != NULL && token[0] != '\0' &&
           rly_http_form_get(req->body, req->body_len, "t", t, sizeof t) && strcmp(t, token) == 0;
}

static size_t do_save(const rly_http_request *req, rly_config *cfg, const char *token,
                      const rly_web_status *st, char *out, size_t cap, rly_web_result *res) {
    rly_config tmp = *cfg;
    char value[RLY_CFG_HOST_MAX];
    char err[96];
    size_t eu = 0;
    err[0] = '\0';

    for (const char *const *k = rly_config_keys; *k != NULL; k++) {
        if (!rly_http_form_get(req->body, req->body_len, *k, value, sizeof value)) {
            continue;
        }
        int secret = strcmp(*k, "mqtt_pass") == 0 || strcmp(*k, "admin_pass") == 0;
        if (secret && value[0] == '\0') {
            continue; /* empty password field = keep the stored one */
        }
        if (strcmp(*k, "admin_pass") == 0) {
            char again[RLY_CFG_HOST_MAX];
            rly_http_form_get(req->body, req->body_len, "admin_pass2", again, sizeof again);
            if (strcmp(value, again) != 0) {
                eu = rly_buf_append(err, sizeof err, eu, "Hasła panelu się różnią. ");
                continue;
            }
        }
        rly_cfg_status s = rly_config_set(&tmp, *k, value);
        if (s != RLY_CFG_OK) {
            eu = rly_buf_append(err, sizeof err, eu, "Błędne pole: ");
            eu = rly_buf_append(err, sizeof err, eu, *k);
            eu = rly_buf_append(err, sizeof err, eu, s == RLY_CFG_TOO_LONG ? " (za długie). "
                                                                           : ". ");
        }
    }
    if (rly_http_form_get(req->body, req->body_len, "mqtt_pass_clear", value, sizeof value) &&
        value[0] == '1') {
        tmp.mqtt_pass[0] = '\0';
    }
    if (err[0] != '\0') {
        return page_settings(&tmp, token, st->fw_version, out, cap, 400, err);
    }

    *cfg = tmp;
    res->action = RLY_WEB_ACT_SAVE_REBOOT;
    char url[64];
    size_t u = rly_buf_append(url, sizeof url, 0, "http://");
    if (!tmp.dhcp) {
        char ip[16];
        rly_format_ipv4(tmp.ip, ip, sizeof ip);
        u = rly_buf_append(url, sizeof url, u, ip);
    } else {
        u = rly_buf_append(url, sizeof url, u, tmp.hostname[0] ? tmp.hostname : st->node_id);
        u = rly_buf_append(url, sizeof url, u, ".local");
    }
    (void)rly_buf_append(url, sizeof url, u, "/");
    return page_message(&tmp, out, cap, "Zapisano. Urządzenie uruchamia się ponownie.", url);
}

static size_t do_relay(const rly_http_request *req, const rly_web_status *st, char *out,
                       size_t cap, rly_web_result *res) {
    char n[8], s[8];
    if (!rly_http_form_get(req->body, req->body_len, "n", n, sizeof n) ||
        !rly_http_form_get(req->body, req->body_len, "s", s, sizeof s)) {
        return simple(out, cap, 400, "missing n/s");
    }
    uint8_t bits;
    if (strcmp(n, "all") == 0) {
        bits = 0xFFu;
    } else if (n[0] >= '1' && n[0] <= '8' && n[1] == '\0') {
        bits = (uint8_t)(1u << (n[0] - '1'));
    } else {
        return simple(out, cap, 400, "bad relay");
    }
    uint8_t mask = st->relay_mask;
    if (strcmp(s, "on") == 0) {
        mask |= bits;
    } else if (strcmp(s, "off") == 0) {
        mask &= (uint8_t)~bits;
    } else if (strcmp(s, "toggle") == 0) {
        mask ^= bits;
    } else {
        return simple(out, cap, 400, "bad state");
    }
    res->action = RLY_WEB_ACT_RELAYS;
    res->relay_mask = mask;
    return redirect_home(out, cap);
}

size_t rly_web_handle(const rly_http_request *req, rly_config *cfg, const rly_web_status *st,
                      const char *token, char *out, size_t cap, rly_web_result *res) {
    res->action = RLY_WEB_ACT_NONE;
    res->relay_mask = st->relay_mask;

    if (!rly_http_check_basic(req->auth, req->auth_len, "admin", cfg->admin_pass)) {
        size_t used = 0;
        A("HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"RLY-8\", "
          "charset=\"UTF-8\"\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
          "Login: admin");
        return used;
    }

    if (req->method == RLY_HTTP_GET) {
        if (strcmp(req->path, "/") == 0) {
            return page_status(cfg, st, token, out, cap, NULL);
        }
        if (strcmp(req->path, "/settings") == 0) {
            return page_settings(cfg, token, st->fw_version, out, cap, 200, NULL);
        }
        if (strcmp(req->path, "/api/status") == 0) {
            return page_json(cfg, st, out, cap);
        }
        return simple(out, cap, 404, "not found");
    }
    if (req->method != RLY_HTTP_POST) {
        return simple(out, cap, 400, "method");
    }
    if (!token_ok(req, token)) {
        return simple(out, cap, 403, "Formularz wygasł – odśwież stronę.");
    }
    if (strcmp(req->path, "/relay") == 0) {
        return do_relay(req, st, out, cap, res);
    }
    if (strcmp(req->path, "/save") == 0) {
        return do_save(req, cfg, token, st, out, cap, res);
    }
    if (strcmp(req->path, "/reboot") == 0) {
        res->action = RLY_WEB_ACT_REBOOT;
        return page_message(cfg, out, cap, "Restart…", "/");
    }
    if (strcmp(req->path, "/factory") == 0) {
        res->action = RLY_WEB_ACT_FACTORY;
        return page_message(cfg, out, cap,
                            "Przywrócono ustawienia fabryczne (DHCP, hasło admin). Restart…",
                            NULL);
    }
    return simple(out, cap, 404, "not found");
}
