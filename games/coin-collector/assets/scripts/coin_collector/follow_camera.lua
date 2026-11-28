-- A smoothed third-person camera that follows an entity by name. Put it on
-- an entity with a camera component.

local Follow = {
    properties = {
        target = "Player", -- name of the entity to follow
        distance = 7,      -- metres behind (+Z of) the target
        height = 4,        -- metres above it
        smoothing = 5,     -- higher follows tighter
    },
}

function Follow:on_start()
    self.target_entity = scene.find(self.target)
    if not self.target_entity then
        log.warn("follow_camera: no entity named '" .. self.target .. "'")
    end
end

function Follow:on_update(dt)
    local target = self.target_entity
    if not (target and target:valid()) then return end
    local focus = target:position() + vec3(0, 1, 0)
    local wanted = target:position() + vec3(0, self.height, self.distance)
    local k = 1 - math.exp(-self.smoothing * dt) -- frame-rate independent smoothing
    self.entity:set_position(self.entity:position():lerp(wanted, k))
    self.entity:look_at(focus)
end

return Follow
