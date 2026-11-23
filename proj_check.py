import math

f = lambda v: round(v, 1)
SW, SH, F = 400.0, 240.0, 220.0
cam = [0.0, 1.3, 12.0]

# landmark: red box dead ahead of spawn
box = [0.0, 0.5, 8.0]

def project(yaw, pitch):
    cY, sY = math.cos(yaw), math.sin(yaw)
    cP, sP = math.cos(pitch), math.sin(pitch)
    R = [cY, 0.0, sY]
    Fv = [sY*cP, sP, -cY*cP]
    U = [-sY*sP, cP, cY*sP]
    rx = box[0]-cam[0]; ry = box[1]-cam[1]; rz = box[2]-cam[2]
    cx = rx*R[0] + ry*R[1] + rz*R[2]
    cy = rx*U[0] + ry*U[1] + rz*U[2]
    cz = rx*Fv[0] + ry*Fv[1] + rz*Fv[2]
    if cz < 0.2:
        return "BEHIND"
    inv = F/cz
    return f(SW/2 + cx*inv), f(SH/2 - cy*inv)

lines = []
lines.append("start  (yaw=0,pitch=0): " + str(project(0.0, 0.0)))
lines.append("Y up   (pitch +0.5):    " + str(project(0.0, 0.5)))
lines.append("A down (pitch -0.5):    " + str(project(0.0, -0.5)))
lines.append("X left (yaw -0.5):      " + str(project(-0.5, 0.0)))
lines.append("B right(yaw +0.5):      " + str(project(0.5, 0.0)))

# hand rest positions (below eye, left/right) -> screen
def hand_screen(offset_right, offset_up, depth):
    hx = cam[0] + offset_right
    hy = cam[1] + offset_up
    hz = cam[2] - depth
    rx = hx-cam[0]; ry = hy-cam[1]; rz = hz-cam[2]
    yaw, pitch = 0.0, 0.0
    cY, sY = math.cos(yaw), math.sin(yaw)
    cP, sP = math.cos(pitch), math.sin(pitch)
    R = [cY, 0.0, sY]
    Fv = [sY*cP, sP, -cY*cP]
    U = [-sY*sP, cP, cY*sP]
    cx = rx*R[0] + ry*R[1] + rz*R[2]
    cy = rx*U[0] + ry*U[1] + rz*U[2]
    cz = rx*Fv[0] + ry*Fv[1] + rz*Fv[2]
    if cz < 0.2:
        return "BEHIND"
    inv = F/cz
    return f(SW/2 + cx*inv), f(SH/2 - cy*inv)

lines.append("hand L base (right -0.35, up -0.30, fwd 1.1): " + str(hand_screen(-0.35, -0.30, 1.1)))
lines.append("hand R base (right +0.35, up -0.30, fwd 1.1): " + str(hand_screen(0.35, -0.30, 1.1)))
lines.append("hand L reach(right -0.35, up -0.90, fwd 1.4): " + str(hand_screen(-0.35, -0.90, 1.4)))

open("proj_check.txt", "w", encoding="utf-8").write("\n".join(lines))
print("done")