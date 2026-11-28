-- Coin Collector: the game manager. Spawns a ring of coins, keeps the score
-- and reports progress to the engine log. Coins spin through the game's
-- native module (module/coin_module.cpp): its coin_spin component, and the
-- ring layout from its Lua module.

local coin_collector = require("coin_collector")

local Game = {
    properties = {
        coins = 8,      -- how many coins to spawn
        radius = 8,     -- ring radius, metres
        coin_value = 1, -- points per coin
    },
}

function Game:on_start()
    self.score = 0
    self.collected = 0
    self.started_at = time.elapsed()
    for i = 1, self.coins do
        scene.spawn({
            name = "Coin",
            transform = {
                position = coin_collector.ring_position(i - 1, self.coins, self.radius),
                scale = vec3(0.6, 0.08, 0.6),
            },
            coin_spin = { spin_speed = 180, bob_height = 0.15 },
            primitive = {
                shape = "cylinder",
                color = "#ffc832",
                metallic = 1,
                roughness = 0.3,
                emissive = vec3(0.6, 0.45, 0.05),
            },
            -- A generous pickup radius: 1.5 x the coin's 0.6 scale = 0.9 m.
            collider = { shape = "sphere", half_extents = vec3(1.5), sensor = true },
            rigidbody = { motion = "kinematic" },
            script = { path = "coin_collector/coin.lua", properties = { value = self.coin_value } },
        })
    end
    print(string.format("Collect all %d coins! WASD / left stick to move, Space / A to jump.",
                        self.coins))
end

-- Called by coin.lua when the player touches a coin.
function Game:collect(value)
    self.collected = self.collected + 1
    self.score = self.score + value
    print(string.format("Score: %d (%d/%d coins)", self.score, self.collected, self.coins))
    if self.collected == self.coins then
        print(string.format("You win! All coins collected in %.1f s.",
                            time.elapsed() - self.started_at))
    end
end

return Game
