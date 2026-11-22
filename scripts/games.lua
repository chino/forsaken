-- vim:set sw=4 ts=4:
--
-- Copyright (C) 2009  Daniel Aquino
--
-- Redistribution and use in source and binary forms, with or without
-- modification, are permitted provided that the following conditions
-- are met:
-- 
-- 1. Redistributions of source code must retain the above copyright
--    notice, this list of conditions and the following disclaimer.
-- 2. Redistributions in binary form must reproduce the above copyright
--    notice, this list of conditions and the following disclaimer in the
--    documentation and/or other materials provided with the distribution.
-- 3. The name of the author may not be used to endorse or promote
--    products derived from this software without specific prior written
--    permission.
-- 
-- THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
-- IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
-- WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
-- ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
-- INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
-- (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
-- SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
-- HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
-- STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
-- IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
-- POSSIBILITY OF SUCH DAMAGE.

local base 	= _G
local table 	= require("table")
local http 	= require("socket.http")
local assert	= assert
local pcall	= pcall
local loadstring	= loadstring

local type	= type

module("games")

local url = "http://fly.thruhere.net/status/games.json"

-- the request blocks the menu while it runs and socket.http defaults to a
-- 60 second timeout, so a dead server froze the join screen (issue #251)
local timeout = 3 -- seconds

function get( url )
	local list = {}
	local old_timeout = http.TIMEOUT
	http.TIMEOUT = timeout
	local ok, body, code = pcall(http.request, url)
	http.TIMEOUT = old_timeout
	if not ok or code ~= 200 or type(body) ~= "string" then return list end
	body = "return " .. body
	local rv,f = pcall(loadstring, body)
	if not (rv and f) then return list end
	local ok, result = pcall(f)
	if not ok or type(result) ~= "table" then return list end
	return result
end

-----------------------------------------------------------------------------
-- C Helpers
----------------------------------------------------------------------------

local list = {}
local last_time = 0
local refresh_time = 1 -- seconds

function update()
	list = get( url )
end

function length()
	return table.maxn(list)
end

-- the C side expects strings, guard against malformed entries
local function field_at( index, field )
	local game = list[index]
	if type(game) ~= "table" or game[field] == nil then return "" end
	return game[field]
end

function name_at( index )
	return field_at( index, "nick" )
end

function ip_at( index )
	return field_at( index, "ip" )
end

function port_at( index )
	return field_at( index, "port" )
end


