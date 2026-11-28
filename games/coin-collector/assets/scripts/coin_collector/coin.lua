-- Coin Collector: a coin. Its coin_spin component (the game's native module)
-- spins and bobs it; when a character touches its sensor it reports to the
-- Game entity and removes itself.

local Coin = {
    properties = {
        value = 1, -- points (the game sets it when spawning)
    },
}

function Coin:on_trigger_enter(other)
    if not other:has("character") then return end
    local game = scene.find("Game")
    if game and game:script() then
        game:script():collect(self.value)
    end
    self.entity:destroy()
end

return Coin
