-- Coin Collector: the player character. WASD / arrows / left stick move
-- (relative to the follow camera), Space / gamepad A jumps.

local Player = {
    properties = {
        jump_speed = 5.5,            -- metres per second, upwards
        respawn = vec3(0, 1, 0),     -- where a fall off the world ends up
    },
}

function Player:on_start()
    self.camera = scene.find("Camera")
end

function Player:on_update(dt)
    local move = input.move_vector() -- x right, z backwards (forward is -Z)
    if self.camera and self.camera:valid() then
        local forward = self.camera:forward()
        forward.y = 0
        if forward:length() > 0.01 then
            forward = forward:normalized()
            local right = vec3(-forward.z, 0, forward.x)
            move = right * move.x - forward * move.z
        end
    end
    self.entity:move(move)

    if input.action_pressed("jump") and self.entity:jump(self.jump_speed) then
        log.debug("jump")
    end

    if self.entity:position().y < -10 then
        self.entity:set_position(self.respawn)
        log.warn("the player fell off the world and was respawned")
    end
end

return Player
