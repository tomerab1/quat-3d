-- Coin Collector: a coin. Spins and bobs; when a character touches its
-- sensor it reports to the Game entity and removes itself.

local Coin = {
    properties = {
        value = 1,         -- points (the game sets it when spawning)
        spin_speed = 180,  -- degrees per second
        bob_height = 0.15, -- metres
    },
}

function Coin:on_start()
    self.base = self.entity:position()
    self.phase = math.random() * 2 * math.pi
    self.angle = 0
end

function Coin:on_update(dt)
    self.phase = self.phase + dt * 2
    self.angle = (self.angle + self.spin_speed * dt) % 360
    self.entity:set_position(self.base + vec3(0, math.sin(self.phase) * self.bob_height, 0))
    -- Stand the coin up (pitch 90) and spin it about the vertical axis.
    self.entity:set_rotation(quat.euler(vec3(90, self.angle, 0)))
end

function Coin:on_trigger_enter(other)
    if not other:has("character") then return end
    local game = scene.find("Game")
    if game and game:script() then
        game:script():collect(self.value)
    end
    self.entity:destroy()
end

return Coin
