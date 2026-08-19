local cjson = require "cjson.safe"

local BenchSecurity = {
  PRIORITY = 1000,
  VERSION = "1.0.0",
}

local function decode_payload(token)
  local _, _, payload = string.find(token or "", "^[^.]+%.([^.]+)%.[^.]+$")
  if not payload then
    return nil
  end
  payload = string.gsub(payload, "-", "+")
  payload = string.gsub(payload, "_", "/")
  payload = payload .. string.rep("=", (4 - #payload % 4) % 4)
  return cjson.decode(ngx.decode_base64(payload) or "")
end

function BenchSecurity:access(conf)
  local target = string.lower(ngx.unescape_uri(
    kong.request.get_path() .. "?" .. (kong.request.get_raw_query() or "")))
  for _, marker in ipairs({ "' or 1=1", "<script", "../", "sqlmap" }) do
    if string.find(target, marker, 1, true) then
      return kong.response.exit(403, { message = "waf" })
    end
  end

  local authorization = kong.request.get_header("authorization") or ""
  local payload = decode_payload(string.match(authorization, "^[Bb]earer%s+(.+)$"))
  local scope = payload and payload.scope or ""
  for item in string.gmatch(scope, "%S+") do
    if item == conf.required_scope then
      return
    end
  end
  return kong.response.exit(403, { message = "scope" })
end

return BenchSecurity
