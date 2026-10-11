/*
 * Interactive hanging-vine sandbox inspired by Animal Well.
 *
 * The original executable's mouse sampler converts the screen cursor into
 * window-client coordinates (GetCursorPos + ScreenToClient). In this portable
 * demo, raylib's GetMousePosition() gives the equivalent client-relative
 * coordinates. The chain solver below is an illustrative reconstruction,
 * rather than a claim that the game uses these exact constants or algorithm.
 *
 * Build on this machine:
 *   cc physics.c -o physics $(pkg-config --cflags --libs raylib) -lm
 */

#include <raylib.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 1100
#define SCREEN_H 720
#define VINE_COUNT 40
#define NODE_COUNT 22

typedef struct Vec2 {
    float x, y;
} Vec2;

typedef struct VineNode {
    Vec2 position;
    Vec2 previous;
} VineNode;

typedef struct Vine {
    VineNode nodes[NODE_COUNT];
    size_t count;
    float segment_length;
    float gravity;
    float damping;
    unsigned constraint_passes;
    Color color;
} Vine;

typedef struct Circle {
    Vec2 center;
    float radius;
} Circle;

static Vec2 v2(float x, float y) { return (Vec2){x, y}; }
static Vec2 add(Vec2 a, Vec2 b) { return v2(a.x + b.x, a.y + b.y); }
static Vec2 sub(Vec2 a, Vec2 b) { return v2(a.x - b.x, a.y - b.y); }
static Vec2 scale(Vec2 a, float s) { return v2(a.x * s, a.y * s); }
static float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
static float length_sq(Vec2 a) { return dot(a, a); }
static Vector2 rv(Vec2 p) { return (Vector2){p.x, p.y}; }

/* Push a node out of a circular body (player or mouse-controlled orb). */
static void collide_node_with_circle(VineNode *node, Circle body)
{
    Vec2 delta = sub(node->position, body.center);
    float d2 = length_sq(delta);
    float min_d2 = body.radius * body.radius;

    if (d2 >= min_d2)
        return;

    if (d2 < 1.0e-8f) {
        delta = v2(0.0f, -1.0f);
        d2 = 1.0f;
    }

    float d = sqrtf(d2);
    Vec2 normal = scale(delta, 1.0f / d);
    node->position = add(body.center, scale(normal, body.radius));

    /* Remove only velocity directed into the collider's surface. */
    Vec2 velocity = sub(node->position, node->previous);
    float into_surface = dot(velocity, normal);
    if (into_surface < 0.0f)
        node->previous = add(node->previous, scale(normal, into_surface));
}

/* Verlet integration plus repeated distance constraints for a hanging chain. */
static void step_vine(Vine *vine, float dt, Circle player,
                      bool mouse_orb_active, Circle mouse_orb)
{
    if (vine->count < 2 || dt <= 0.0f)
        return;

    float dt2 = dt * dt;
    for (size_t i = 1; i < vine->count; ++i) {
        VineNode *node = &vine->nodes[i];
        Vec2 old = node->position;
        Vec2 velocity = scale(sub(node->position, node->previous), vine->damping);
        node->position = add(add(node->position, velocity),
                             v2(0.0f, vine->gravity * dt2));
        node->previous = old;
    }

    for (unsigned pass = 0; pass < vine->constraint_passes; ++pass) {
        for (size_t i = 0; i + 1 < vine->count; ++i) {
            VineNode *a = &vine->nodes[i];
            VineNode *b = &vine->nodes[i + 1];
            Vec2 delta = sub(b->position, a->position);
            float d2 = length_sq(delta);
            if (d2 < 1.0e-8f)
                continue;

            float d = sqrtf(d2);
            Vec2 correction = scale(delta, (d - vine->segment_length) / d);
            if (i == 0) {
                /* The top attachment is fixed in place. */
                b->position = sub(b->position, correction);
            } else {
                a->position = add(a->position, scale(correction, 0.5f));
                b->position = sub(b->position, scale(correction, 0.5f));
            }
        }

        for (size_t i = 1; i < vine->count; ++i) {
            collide_node_with_circle(&vine->nodes[i], player);
            if (mouse_orb_active)
                collide_node_with_circle(&vine->nodes[i], mouse_orb);
        }
    }
}

static const Color vine_colors[VINE_COUNT] = {
    {105, 181, 87, 255}, {138, 194, 92, 255}, {91, 167, 99, 255},
    {164, 190, 91, 255}, {91, 178, 127, 255}
};

static void reset_vines(Vine vines[VINE_COUNT])
{
    float screen_width = (float)GetScreenWidth();
    for (int v = 0; v < VINE_COUNT; ++v) {
        Vine *vine = &vines[v];
        vine->count = NODE_COUNT;
        vine->segment_length = 14.0f + (float)((v * 3) % 7);
        vine->gravity = 1700.0f;
        vine->damping = 0.992f;
        vine->constraint_passes = 8;
        vine->color = vine_colors[v % (int)(sizeof(vine_colors) / sizeof(vine_colors[0]))];

        float x = 24.0f + (screen_width - 48.0f) * (float)v / (float)(VINE_COUNT - 1);
        float y = 105.0f + (float)((v * 17) % 6) * 11.0f;
        for (size_t i = 0; i < vine->count; ++i) {
            float sway = sinf((float)i * 0.42f + (float)v) * (float)i * 0.45f;
            vine->nodes[i].position = v2(x + sway, y + (float)i * vine->segment_length);
            vine->nodes[i].previous = vine->nodes[i].position;
        }
    }
}

static void draw_vine(const Vine *vine)
{
    for (size_t i = 0; i + 1 < vine->count; ++i) {
        Vec2 a = vine->nodes[i].position;
        Vec2 b = vine->nodes[i + 1].position;
        float width = 3.7f;
        DrawLineEx(rv(a), rv(b), width, (Color){35, 70, 46, 255});
        DrawLineEx(rv(a), rv(b), width - 2.0f, vine->color);

        /* Small paired leaves make the chain read as a hanging plant. */
        if (i > 1 && i % 3 == 0) {
            Vec2 tangent = sub(b, a);
            float len = sqrtf(length_sq(tangent));
            if (len > 0.01f) {
                Vec2 side = v2(-tangent.y / len, tangent.x / len);
                Vec2 center = add(a, scale(tangent, 0.58f));
                Vec2 tip1 = add(center, scale(side, 10.0f));
                Vec2 tip2 = sub(center, scale(side, 10.0f));
                Vec2 end1 = add(tip1, scale(tangent, 0.34f));
                Vec2 end2 = add(tip2, scale(tangent, 0.34f));
                DrawTriangle(rv(center), rv(tip1), rv(end1), (Color){127, 193, 91, 255});
                DrawTriangle(rv(center), rv(tip2), rv(end2), (Color){94, 163, 81, 255});
            }
        }
    }

    DrawCircleV(rv(vine->nodes[0].position), 6.0f, (Color){202, 188, 121, 255});
    DrawCircleV(rv(vine->nodes[vine->count - 1].position), 4.0f, vine->color);
}

static void update_player(Circle *player, float dt)
{
    Vec2 direction = v2(0.0f, 0.0f);
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  direction.x -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) direction.x += 1.0f;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    direction.y -= 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  direction.y += 1.0f;

    float len2 = length_sq(direction);
    if (len2 > 0.0f)
        direction = scale(direction, 1.0f / sqrtf(len2));
    player->center = add(player->center, scale(direction, 250.0f * dt));

    float w = (float)GetScreenWidth();
    float h = (float)GetScreenHeight();
    if (player->center.x < player->radius) player->center.x = player->radius;
    if (player->center.y < player->radius + 55.0f) player->center.y = player->radius + 55.0f;
    if (player->center.x > w - player->radius) player->center.x = w - player->radius;
    if (player->center.y > h - player->radius) player->center.y = h - player->radius;
}

int main(void)
{
    InitWindow(SCREEN_W, SCREEN_H, "Animal Well - Vine Physics Sandbox");
    if (!IsWindowReady())
        return 1; /* For example, no accessible desktop display. */
    SetWindowMinSize(700, 480);
    SetTargetFPS(120);

    Vine vines[VINE_COUNT] = {0};
    reset_vines(vines);
    Circle player = { {SCREEN_W * 0.5f, SCREEN_H * 0.70f}, 19.0f };
    bool paused = false;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;

        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_R)) {
            reset_vines(vines);
            player.center = v2((float)GetScreenWidth() * 0.5f,
                               (float)GetScreenHeight() * 0.70f);
        }

        Vec2 mouse = v2(GetMousePosition().x, GetMousePosition().y);
        bool mouse_orb_active = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        Circle mouse_orb = { mouse, 17.0f };

        if (!paused) {
            update_player(&player, dt);
            for (int v = 0; v < VINE_COUNT; ++v)
                step_vine(&vines[v], dt, player, mouse_orb_active, mouse_orb);
        }

        BeginDrawing();
        ClearBackground((Color){18, 27, 31, 255});
        DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                               (Color){28, 43, 43, 255}, (Color){15, 22, 29, 255});

        /* A soft ground line helps show the motion and scale. */
        DrawLineEx((Vector2){25.0f, (float)GetScreenHeight() - 28.0f},
                   (Vector2){(float)GetScreenWidth() - 25.0f,
                             (float)GetScreenHeight() - 28.0f},
                   2.0f, (Color){74, 99, 79, 255});

        for (int v = 0; v < VINE_COUNT; ++v)
            draw_vine(&vines[v]);

        DrawCircleV(rv(player.center), player.radius + 4.0f, (Color){12, 22, 26, 210});
        DrawCircleV(rv(player.center), player.radius, (Color){217, 190, 123, 255});
        DrawCircleV((Vector2){player.center.x - 5.0f, player.center.y - 5.0f},
                    4.0f, (Color){245, 224, 164, 255});

        if (mouse_orb_active) {
            DrawCircleV(rv(mouse_orb.center), mouse_orb.radius + 5.0f,
                        (Color){96, 192, 171, 75});
            DrawCircleV(rv(mouse_orb.center), mouse_orb.radius,
                        (Color){104, 211, 185, 235});
            DrawCircleV(rv(mouse_orb.center), 5.0f,
                        (Color){224, 255, 217, 255});
        }

        DrawRectangle(0, 0, GetScreenWidth(), 62, (Color){8, 15, 18, 205});
        DrawText("VINE PHYSICS SANDBOX", 20, 13, 23, (Color){224, 230, 205, 255});
        DrawText("Hold left mouse for a physics orb   |   Move: WASD / arrows   |   Space: pause   |   R: reset",
                 21, 39, 15, (Color){167, 190, 173, 255});
        if (paused)
            DrawText("PAUSED", GetScreenWidth() - 105, 17, 20, (Color){242, 208, 119, 255});
        DrawText("Vines use Verlet motion, length constraints, and player contact.",
                 20, GetScreenHeight() - 22, 14, (Color){150, 170, 161, 255});
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
