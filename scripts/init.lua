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

-- dofile that looks in proper folder
function dofile(filename)
	path="./scripts/"..filename
	local f = assert(loadfile(path))
	return f()
end

-- startup tasks
function init( debug )

	-- tell require() to look in ./Scripts/*
	--
	-- the bundled socket/mime/ltn12 lua modules are the 2.0 series and
	-- only work with a 2.0 socket.core.  when we were linked against a
	-- newer system luasocket keep its own lua modules ahead of ours.
	local core = require("socket.core")
	local version = core and core._VERSION or ""
	-- luasocket 2.0 registers its core as "socket" too, which would
	-- stop require("socket") from loading socket.lua
	if package.loaded.socket == core then package.loaded.socket = nil end
	if string.match(version, "^LuaSocket 2%.") then
		package.path = "./scripts/?.lua;" .. package.path
	else
		package.path = package.path .. ";./scripts/?.lua"
	end

	-- globals
	config = {} -- main config table

	-- load modules
	games = require("games")

	-- load library files
        dofile("extensions.lua")
        dofile("helpers.lua")
        dofile("config.lua")

	-- load config
	if debug then
		config_load("debug")
	else
		config_load("main")
	end

end

