#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 3000
#define REQUEST_CAP 65536
#define RESPONSE_CAP 131072
#define VALUE_CAP 512
#define TEXT_CAP 2048

typedef struct {
    double income;
    double recurring;
    double flexible;
    double debt;
    double savings;
    double goal;
} Budget;

typedef struct {
    double committed;
    double remaining;
    double savings_rate;
    double suggested;
    double goal_months;
} Snapshot;

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
} Html;

static void html_init(Html *html, char *buffer, size_t capacity) {
    html->data = buffer;
    html->used = 0;
    html->capacity = capacity;
    html->data[0] = '\0';
}

static void html_append(Html *html, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void html_append(Html *html, const char *format, ...) {
    if (html->used >= html->capacity) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(html->data + html->used, html->capacity - html->used, format, args);
    va_end(args);
    if (written > 0) {
        size_t amount = (size_t) written;
        html->used += amount < html->capacity - html->used ? amount : html->capacity - html->used - 1;
    }
}

static void html_escape(const char *source, char *destination, size_t capacity) {
    size_t used = 0;
    if (capacity == 0) return;
    for (size_t i = 0; source && source[i] != '\0' && used + 1 < capacity; i++) {
        const char *replacement = NULL;
        switch (source[i]) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement) {
            size_t length = strlen(replacement);
            if (used + length + 1 >= capacity) break;
            memcpy(destination + used, replacement, length);
            used += length;
        } else {
            destination[used++] = source[i];
        }
    }
    destination[used] = '\0';
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static void url_decode(const char *source, char *destination, size_t capacity) {
    size_t used = 0;
    if (capacity == 0) return;
    for (size_t i = 0; source && source[i] != '\0' && used + 1 < capacity; i++) {
        if (source[i] == '+') {
            destination[used++] = ' ';
        } else if (source[i] == '%' && source[i + 1] && source[i + 2]) {
            int high = hex_value(source[i + 1]);
            int low = hex_value(source[i + 2]);
            if (high >= 0 && low >= 0) {
                destination[used++] = (char)((high << 4) | low);
                i += 2;
            } else {
                destination[used++] = source[i];
            }
        } else {
            destination[used++] = source[i];
        }
    }
    destination[used] = '\0';
}

static bool form_value(const char *body, const char *wanted, char *output, size_t capacity) {
    if (!body || !wanted || capacity == 0) return false;
    const char *cursor = body;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t segment_length = end ? (size_t)(end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', segment_length);
        if (equals) {
            char key[VALUE_CAP];
            size_t key_length = (size_t)(equals - cursor);
            if (key_length >= sizeof(key)) key_length = sizeof(key) - 1;
            memcpy(key, cursor, key_length);
            key[key_length] = '\0';
            char decoded_key[VALUE_CAP];
            url_decode(key, decoded_key, sizeof(decoded_key));
            if (strcmp(decoded_key, wanted) == 0) {
                size_t value_length = segment_length - (size_t)(equals - cursor) - 1;
                char raw[VALUE_CAP];
                if (value_length >= sizeof(raw)) value_length = sizeof(raw) - 1;
                memcpy(raw, equals + 1, value_length);
                raw[value_length] = '\0';
                url_decode(raw, output, capacity);
                return true;
            }
        }
        if (!end) break;
        cursor = end + 1;
    }
    output[0] = '\0';
    return false;
}

static bool parse_money(const char *text, double *value) {
    if (!text || !*text) return false;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    while (end && isspace((unsigned char)*end)) end++;
    if (errno == ERANGE || end == text || (end && *end != '\0') || !isfinite(parsed) || parsed < 0.0) return false;
    *value = parsed;
    return true;
}

static double money_floor(double value) {
    return floor(value * 100.0 + 0.00001) / 100.0;
}

static Snapshot calculate_snapshot(const Budget *budget) {
    Snapshot snapshot;
    snapshot.committed = budget->recurring + budget->flexible + budget->debt;
    snapshot.remaining = budget->income - snapshot.committed;
    snapshot.savings_rate = budget->income > 0.0 ? (budget->savings / budget->income) * 100.0 : 0.0;
    snapshot.suggested = snapshot.remaining > 0.0 ? snapshot.remaining * 0.50 : 0.0;
    snapshot.goal_months = snapshot.suggested > 0.0 && budget->goal > 0.0 ? budget->goal / snapshot.suggested : 0.0;
    return snapshot;
}

static const char *budget_status(const Snapshot *snapshot) {
    if (snapshot->remaining < 0.0) return "Needs a reset";
    if (snapshot->remaining < 100.0) return "Very little breathing room";
    if (snapshot->remaining < 500.0) return "A workable starting point";
    return "Healthy breathing room";
}

static void money_text(double value, char *output, size_t capacity) {
    snprintf(output, capacity, "$%.2f", money_floor(value));
}

static double percent_width(double value, double total) {
    if (total <= 0.0 || value <= 0.0) return 0.0;
    double width = (value / total) * 100.0;
    return width > 100.0 ? 100.0 : width;
}

static void page_start(Html *html, const char *eyebrow, const char *title, const char *intro) {
    html_append(html, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><meta name=\"description\" content=\"Centsage is a calm, educational personal finance advisor powered by C.\"><title>%s · Centsage</title><link rel=\"stylesheet\" href=\"/styles.css\"></head><body>", title);
    html_append(html, "<div class=\"shell\"><aside class=\"rail\"><a class=\"brand\" href=\"/\" aria-label=\"Centsage home\"><span class=\"brand-mark\"><span>C</span><span>S</span></span><span class=\"brand-name\">centsage</span></a><div class=\"rail-rule\"></div><p class=\"rail-note\">A calmer first conversation with your money.</p><div class=\"rail-bottom\"><span class=\"status-dot\"></span><span>Session only<br><small>Nothing is saved</small></span></div></aside><main class=\"main\"><header class=\"page-heading\"><p class=\"eyebrow\">%s</p><h1>%s</h1><p class=\"lede\">%s</p></header>", eyebrow, title, intro);
}

static void page_end(Html *html) {
    html_append(html, "<footer class=\"footer\"><span>Centsage is an educational tool, not a licensed financial advisor.</span><span>Your entries are processed for this session only.</span></footer></main></div></body></html>");
}

static void budget_form(Html *html, const Budget *budget, const char *error) {
    char income[40], recurring[40], flexible[40], debt[40], savings[40], goal[40], escaped_error[TEXT_CAP];
    snprintf(income, sizeof(income), "%.2f", budget->income);
    snprintf(recurring, sizeof(recurring), "%.2f", budget->recurring);
    snprintf(flexible, sizeof(flexible), "%.2f", budget->flexible);
    snprintf(debt, sizeof(debt), "%.2f", budget->debt);
    snprintf(savings, sizeof(savings), "%.2f", budget->savings);
    snprintf(goal, sizeof(goal), "%.2f", budget->goal);
    html_escape(error ? error : "", escaped_error, sizeof(escaped_error));
    html_append(html, "<section class=\"input-panel\" id=\"budget\"><div class=\"section-kicker\"><span class=\"step\">01</span><span>Set the month</span></div><h2>Start with the shape of your month.</h2><p class=\"section-copy\">Use take-home numbers, not gross pay. Rounded estimates are completely fine.</p>");
    if (error && *error) html_append(html, "<div class=\"form-alert\" role=\"alert\"><strong>One quick fix:</strong> %s</div>", escaped_error);
    html_append(html, "<form method=\"post\" action=\"/api/plan\" class=\"budget-form\"><div class=\"field-grid\"><label>Monthly take-home income <span class=\"hint\">after tax</span><span class=\"money-input\"><span>$</span><input name=\"income\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"4200\" required></span></label><label>Recurring essentials <span class=\"hint\">rent, utilities, subscriptions</span><span class=\"money-input\"><span>$</span><input name=\"recurring\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"1400\" required></span></label><label>Flexible spending <span class=\"hint\">food, fun, transport</span><span class=\"money-input\"><span>$</span><input name=\"flexible\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"550\" required></span></label><label>Debt payments <span class=\"hint\">minimums plus extra</span><span class=\"money-input\"><span>$</span><input name=\"debt\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"350\" required></span></label><label>Current savings <span class=\"hint\">your starting cushion</span><span class=\"money-input\"><span>$</span><input name=\"savings\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"2400\" required></span></label><label>Savings goal <span class=\"hint\">the next milestone</span><span class=\"money-input\"><span>$</span><input name=\"goal\" inputmode=\"decimal\" type=\"number\" min=\"0\" step=\"0.01\" value=\"%s\" placeholder=\"3000\" required></span></label></div><div class=\"form-actions\"><button class=\"button button-primary\" type=\"submit\">Read my month <span aria-hidden=\"true\">→</span></button><span class=\"microcopy\">No account. No bank connection. No saved inputs.</span></div></form></section>", income, recurring, flexible, debt, savings, goal);
}

static void render_advisor(Html *html, const char *answer, const char *question) {
    char safe_answer[TEXT_CAP], safe_question[TEXT_CAP];
    html_escape(answer ? answer : "", safe_answer, sizeof(safe_answer));
    html_escape(question ? question : "", safe_question, sizeof(safe_question));
    html_append(html, "<section class=\"advisor-panel\" id=\"advisor\"><div class=\"section-kicker\"><span class=\"step\">03</span><span>Ask the guide</span></div><div class=\"advisor-heading\"><div><h2>Bring one money question.</h2><p class=\"section-copy\">Try “How do I start?” or “Should I build savings before paying extra on debt?”</p></div><span class=\"advisor-mark\">C</span></div>");
    if (answer && *answer) html_append(html, "<div class=\"answer\"><p class=\"answer-label\">Centsage says</p><p>%s</p><p class=\"answer-note\">This is a general educational starting point, not individualized professional advice.</p></div>", safe_answer);
    html_append(html, "<form method=\"post\" action=\"/api/advice\" class=\"advisor-form\"><label for=\"question\">Your question</label><div class=\"question-row\"><input id=\"question\" name=\"question\" value=\"%s\" placeholder=\"What should I focus on first?\" maxlength=\"500\" required><button class=\"button button-dark\" type=\"submit\">Ask Centsage <span aria-hidden=\"true\">↗</span></button></div></form><div class=\"suggestions\"><span>Try asking:</span><a href=\"/\" aria-label=\"Ask how to start budgeting\">How do I start budgeting?</a><a href=\"/\" aria-label=\"Ask about an emergency buffer\">What is a starter buffer?</a><a href=\"/\" aria-label=\"Ask about debt and savings\">Debt or savings first?</a></div></section>", safe_question);
}

static void render_home(Html *html, const char *error) {
    Budget blank = {0};
    page_start(html, "PERSONAL FINANCE / 01", "See what your month can carry.", "A simple, private budgeting conversation. Put in a few monthly numbers and get a clearer next move — without handing over your accounts.");
    budget_form(html, &blank, error);
    html_append(html, "<section class=\"how-panel\"><div class=\"section-kicker\"><span class=\"step\">02</span><span>What happens next</span></div><div class=\"how-grid\"><div><strong>We total the month.</strong><p>Income minus essentials, flexible spending, and debt payments.</p></div><div><strong>We name the margin.</strong><p>See what remains, your current savings rate, and a simple breakdown.</p></div><div><strong>We suggest one move.</strong><p>A practical contribution range based on the numbers you shared.</p></div></div></section>");
    render_advisor(html, NULL, NULL);
    page_end(html);
}

static void render_snapshot(Html *html, const Budget *budget) {
    Snapshot snapshot = calculate_snapshot(budget);
    char remaining[40], committed[40], suggested[40], current[40], goal[40];
    money_text(snapshot.remaining, remaining, sizeof(remaining));
    money_text(snapshot.committed, committed, sizeof(committed));
    money_text(snapshot.suggested, suggested, sizeof(suggested));
    money_text(budget->savings, current, sizeof(current));
    money_text(budget->goal, goal, sizeof(goal));
    const char *status = budget_status(&snapshot);
    double chart_total = budget->income > 0.0 ? budget->income : 1.0;
    double recurring_width = percent_width(budget->recurring, chart_total);
    double flexible_width = percent_width(budget->flexible, chart_total);
    double debt_width = percent_width(budget->debt, chart_total);
    double remaining_width = percent_width(snapshot.remaining > 0.0 ? snapshot.remaining : 0.0, chart_total);
    char recurring_text[40];
    money_text(budget->recurring, recurring_text, sizeof(recurring_text));
    page_start(html, "PERSONAL FINANCE / YOUR SNAPSHOT", "Here is what your month is saying.", "The goal is not a perfect budget. It is a clear enough picture to choose the next useful move.");
    html_append(html, "<section class=\"snapshot-hero\"><div><p class=\"eyebrow accent\">MONTHLY BREATHING ROOM</p><div class=\"hero-number %s\">%s</div><p class=\"hero-caption\">after recurring costs, flexible spending, and debt payments</p></div><div class=\"status-card\"><span class=\"status-label\">YOUR READ</span><strong>%s</strong><p>%s</p></div></section>", snapshot.remaining >= 0 ? "positive" : "negative", remaining, status, snapshot.remaining >= 0 ? "There is a little space to direct with intention." : "Your commitments are above income, so the first move is to find a reset point.");
    html_append(html, "<section class=\"metric-strip\"><div><span>Monthly committed</span><strong>%s</strong><small>%.1f%% of income</small></div><div><span>Current savings rate</span><strong>%.1f%%</strong><small>based on your current savings</small></div><div><span>Suggested next save</span><strong>%s</strong><small>half of positive margin</small></div></section>", committed, budget->income > 0 ? (snapshot.committed / budget->income) * 100.0 : 0.0, snapshot.savings_rate, suggested);
    html_append(html, "<section class=\"breakdown-panel\"><div class=\"section-kicker\"><span class=\"step\">01</span><span>The shape of your month</span></div><div class=\"breakdown-heading\"><h2>Every dollar has a lane.</h2><p>These bars compare each lane with your take-home income.</p></div><div class=\"bar-list\"><div class=\"bar-row\"><div><span>Recurring essentials</span><strong>%s</strong></div><div class=\"bar-track\"><span class=\"bar recurring\" style=\"width: %.1f%%\"></span></div></div><div class=\"bar-row\"><div><span>Flexible spending</span><strong>$%.2f</strong></div><div class=\"bar-track\"><span class=\"bar flexible\" style=\"width: %.1f%%\"></span></div></div><div class=\"bar-row\"><div><span>Debt payments</span><strong>$%.2f</strong></div><div class=\"bar-track\"><span class=\"bar debt\" style=\"width: %.1f%%\"></span></div></div><div class=\"bar-row\"><div><span>Left to direct</span><strong>%s</strong></div><div class=\"bar-track\"><span class=\"bar left\" style=\"width: %.1f%%\"></span></div></div></div><p class=\"calculation-note\"><span class=\"note-mark\">i</span><span><strong>How this is calculated:</strong> take-home income minus recurring essentials, flexible spending, and debt payments. Centsage uses simple monthly arithmetic and keeps your entries in this session only.</span></p></section>", recurring_text, recurring_width, budget->flexible, flexible_width, budget->debt, debt_width, remaining, remaining_width);
    html_append(html, "<section class=\"recommendation-panel\"><div class=\"recommendation-copy\"><div class=\"section-kicker\"><span class=\"step\">02</span><span>A reasonable next move</span></div><h2>Give your margin a job.</h2><p>Based on your current numbers, a gentle starting point is to direct <strong>%s per month</strong> toward your goal, then revisit after one full month.</p><p class=\"why\"><strong>Why this appears:</strong> Centsage uses half of positive remaining money so the plan leaves room for real life instead of treating every dollar as already spoken for.</p></div><div class=\"goal-card\"><span class=\"goal-label\">YOUR GOAL</span><strong>%s</strong><span class=\"goal-meta\">starting from %s</span></div></section>", suggested, goal, current);
    if (snapshot.goal_months > 0.0) {
        char months[40];
        snprintf(months, sizeof(months), "%.1f months at that pace", snapshot.goal_months);
        html_append(html, "<p class=\"goal-timeline\">%s</p>", months);
    }
    render_advisor(html, NULL, NULL);
    page_end(html);
}

static const char *advisor_answer(const char *question) {
    if (!question || !*question) return "Start with the numbers you know. A useful first budget is a draft, not a verdict: income, essential commitments, flexible spending, and debt payments are enough to begin.";
    char lower[TEXT_CAP];
    size_t length = strlen(question);
    if (length >= sizeof(lower)) length = sizeof(lower) - 1;
    for (size_t i = 0; i < length; i++) lower[i] = (char)tolower((unsigned char)question[i]);
    lower[length] = '\0';
    if (strstr(lower, "debt") || strstr(lower, "loan") || strstr(lower, "credit card")) {
        return "Keep minimum payments current first, then create a small cash buffer you can actually keep. After that, compare the interest cost and emotional weight of each debt before choosing where extra money goes. This tool cannot tell you which payoff strategy is best for your circumstances, but it can help you make the trade-off visible.";
    }
    if (strstr(lower, "emergency") || strstr(lower, "buffer") || strstr(lower, "rainy")) {
        return "A starter buffer is a small, clearly named cash reserve for an expected surprise: a repair, a gap between paychecks, or a necessary trip. Pick an amount that feels reachable, keep it separate from everyday spending, and automate a modest contribution if that is practical.";
    }
    if (strstr(lower, "start") || strstr(lower, "budget") || strstr(lower, "begin")) {
        return "Start with one month of take-home income and four lanes: recurring essentials, flexible spending, debt payments, and money left to direct. Track only what will change a decision. A budget becomes useful when it helps you notice a pattern early, not when every line is perfect.";
    }
    if (strstr(lower, "save") || strstr(lower, "goal") || strstr(lower, "saving")) {
        return "Name one near-term goal, give it a dollar amount, and choose a contribution that leaves some breathing room. A smaller amount you can repeat usually teaches you more than a heroic amount you abandon. Review the pace monthly and adjust when your real life changes.";
    }
    return "Try making the question smaller: what is the next money decision you want to feel clearer about? Start with a monthly number, name the trade-off, and choose a move you could repeat. Centsage offers general education, not professional financial advice.";
}

static bool parse_budget(const char *body, Budget *budget, char *error, size_t error_capacity) {
    const char *keys[] = {"income", "recurring", "flexible", "debt", "savings", "goal"};
    double *values[] = {&budget->income, &budget->recurring, &budget->flexible, &budget->debt, &budget->savings, &budget->goal};
    for (size_t i = 0; i < 6; i++) {
        char raw[VALUE_CAP];
        if (!form_value(body, keys[i], raw, sizeof(raw)) || !parse_money(raw, values[i])) {
            snprintf(error, error_capacity, "%s needs a number of zero or more.", keys[i]);
            return false;
        }
    }
    if (budget->income <= 0.0) {
        snprintf(error, error_capacity, "Monthly take-home income needs to be greater than zero.");
        return false;
    }
    return true;
}

static void send_response(int client, int status, const char *content_type, const char *body) {
    const char *status_text = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Bad Request");
    char header[512];
    int length = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, status_text, content_type, strlen(body));
    send(client, header, (size_t)length, 0);
    send(client, body, strlen(body), 0);
}

static void handle_client(int client) {
    char request[REQUEST_CAP];
    size_t received = 0;
    while (received + 1 < sizeof(request)) {
        ssize_t count = recv(client, request + received, sizeof(request) - received - 1, 0);
        if (count <= 0) break;
        received += (size_t)count;
        request[received] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    if (received == 0) return;
    char method[16] = {0};
    char path[256] = {0};
    if (sscanf(request, "%15s %255s", method, path) != 2) return;
    char *body = strstr(request, "\r\n\r\n");
    body = body ? body + 4 : request + received;
    int content_length = 0;
    char *length_header = strstr(request, "Content-Length:");
    if (length_header) sscanf(length_header, "Content-Length: %d", &content_length);
    size_t body_received = received - (size_t)(body - request);
    while (strcmp(method, "POST") == 0 && content_length > 0 && body_received < (size_t)content_length && received + 1 < sizeof(request)) {
        ssize_t count = recv(client, request + received, sizeof(request) - received - 1, 0);
        if (count <= 0) break;
        received += (size_t)count;
        request[received] = '\0';
        body = strstr(request, "\r\n\r\n");
        body = body ? body + 4 : request + received;
        body_received = received - (size_t)(body - request);
    }
    char response[RESPONSE_CAP];
    Html html;
    html_init(&html, response, sizeof(response));
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        render_home(&html, NULL);
        send_response(client, 200, "text/html; charset=utf-8", response);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        send_response(client, 200, "text/plain; charset=utf-8", "ok\n");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/manus-routes.json") == 0) {
        send_response(client, 200, "application/json; charset=utf-8", "{\"routes\":[{\"path\":\"/\",\"title\":\"Centsage Personal Finance Advisor\"}]}");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/styles.css") == 0) {
        FILE *file = fopen("public/styles.css", "rb");
        if (!file) {
            send_response(client, 404, "text/plain; charset=utf-8", "Not found\n");
        } else {
            char css[65536];
            size_t length = fread(css, 1, sizeof(css) - 1, file);
            fclose(file);
            css[length] = '\0';
            send_response(client, 200, "text/css; charset=utf-8", css);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/plan") == 0) {
        Budget budget = {0};
        char error[TEXT_CAP];
        if (!parse_budget(body, &budget, error, sizeof(error))) {
            render_home(&html, error);
            send_response(client, 400, "text/html; charset=utf-8", response);
        } else {
            render_snapshot(&html, &budget);
            send_response(client, 200, "text/html; charset=utf-8", response);
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/advice") == 0) {
        char question[TEXT_CAP];
        form_value(body, "question", question, sizeof(question));
        page_start(&html, "PERSONAL FINANCE / ASK THE GUIDE", "Start with the next useful move.", "A short, plain-language response for a common budgeting or savings question.");
        render_advisor(&html, advisor_answer(question), question);
        page_end(&html);
        send_response(client, 200, "text/html; charset=utf-8", response);
    } else {
        const char *not_found = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Not found · Centsage</title><link rel=\"stylesheet\" href=\"/styles.css\"></head><body><main class=\"not-found\"><p class=\"eyebrow\">404 / NOT IN THE LEDGER</p><h1>That page is not here.</h1><a class=\"button button-dark\" href=\"/\">Return to Centsage</a></main></body></html>";
        send_response(client, 404, "text/html; charset=utf-8", not_found);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const char *port_text = getenv("PORT");
    int port = port_text ? atoi(port_text) : DEFAULT_PORT;
    if (port < 1024 || port > 65535) port = DEFAULT_PORT;
    address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server);
        return 1;
    }
    if (listen(server, 32) < 0) {
        perror("listen");
        close(server);
        return 1;
    }
    fprintf(stdout, "Centsage listening on 0.0.0.0:%d\n", port);
    fflush(stdout);
    while (true) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(client);
        close(client);
    }
    close(server);
    return 0;
}
