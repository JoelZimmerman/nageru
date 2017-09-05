-- The theme is what decides what's actually shown on screen, what kind of
-- transitions are available (if any), and what kind of inputs there are,
-- if any. In general, it drives the entire display logic by creating Movit
-- chains, setting their parameters and then deciding which to show when.
--
-- Themes are written in Lua, which reflects a simplified form of the Movit API
-- where all the low-level details (such as texture formats) are handled by the
-- C++ side and you generally just build chains.
--
-- This is a much simpler theme than the default theme; it only allows you to
-- switch between inputs and set white balance, no transitions or the likes.
-- Thus, it should be simpler to understand.

local input_neutral_color = {{0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}}

local live_signal_num = 0
local preview_signal_num = 1

-- A chain to show a single input, with white balance. In a real example,
-- we'd probably want to support deinterlacing and high-quality scaling
-- (if the input isn't exactly what we want). However, we don't want these
-- things always on, so we'd need to generate more chains for the various
-- cases. In such a simple example, just having two is fine.
function make_simple_chain(hq)
	local chain = EffectChain.new(16, 9)

	local input = chain:add_live_input(false, false)  -- No deinterlacing, no bounce override.
	input:connect_signal(0)  -- First input card. Can be changed whenever you want.
	local wb_effect = chain:add_effect(WhiteBalanceEffect.new())
	chain:finalize(hq)

	return {
		chain = chain,
		input = input,
		wb_effect = wb_effect,
	}
end

-- We only make two chains; one for the live view and one for the previews.
-- (Since they have different outputs, you cannot mix and match them.)
local simple_hq_chain = make_simple_chain(true)
local simple_lq_chain = make_simple_chain(false)

-- API ENTRY POINT
-- Returns the number of outputs in addition to the live (0) and preview (1).
-- Called only once, at the start of the program.
function num_channels()
	return 2
end

-- API ENTRY POINT
-- Returns the name for each additional channel (starting from 2).
-- Called at the start of the program, and then each frame for live
-- channels in case they change resolution.
function channel_name(channel)
	if channel == 2 then
		return "First input"
	elseif channel == 3 then
		return "Second input"
	end
end

-- API ENTRY POINT
-- Returns, given a channel number, which signal it corresponds to (starting from 0).
-- Should return -1 if the channel does not correspond to a simple signal.
-- (The information is used for whether right-click on the channel should bring up
-- an input selector or not.)
-- Called once for each channel, at the start of the program.
-- Will never be called for live (0) or preview (1).
function channel_signal(channel)
	if channel == 2 then
		return 0
	elseif channel == 3 then
		return 1
	else
		return -1
	end
end

-- API ENTRY POINT
-- Called every frame. Returns the color (if any) to paint around the given
-- channel. Returns a CSS color (typically to mark live and preview signals);
-- "transparent" is allowed.
-- Will never be called for live (0) or preview (1).
function channel_color(channel)
	return "transparent"
end

-- API ENTRY POINT
-- Returns if a given channel supports setting white balance (starting from 2).
-- Called only once for each channel, at the start of the program.
function supports_set_wb(channel)
	return channel == 2 or channel == 3
end

-- API ENTRY POINT
-- Gets called with a new gray point when the white balance is changing.
-- The color is in linear light (not sRGB gamma).
function set_wb(channel, red, green, blue)
	if channel == 2 then
		input_neutral_color[1] = { red, green, blue }
	elseif channel == 3 then
		input_neutral_color[2] = { red, green, blue }
	end
end

-- API ENTRY POINT
-- Called every frame.
function get_transitions(t)
	if live_signal_num == preview_signal_num then
		-- No transitions possible.
		return {}
	else
		return {"Cut"}
	end
end

-- API ENTRY POINT
-- Called when the user clicks a transition button. For our case,
-- we only do cuts, so we ignore the parameters; just switch live and preview.
function transition_clicked(num, t)
	local temp = live_signal_num
	live_signal_num = preview_signal_num
	preview_signal_num = temp
end

-- API ENTRY POINT
function channel_clicked(num)
	preview_signal_num = num
end

-- API ENTRY POINT
-- Called every frame. Get the chain for displaying at input <num>,
-- where 0 is live, 1 is preview, 2 is the first channel to display
-- in the bottom bar, and so on up to num_channels()+1. t is the
-- current time in seconds. width and height are the dimensions of
-- the output, although you can ignore them if you don't need them
-- (they're useful if you want to e.g. know what to resample by).
--
-- <signals> is basically an exposed InputState, which you can use to
-- query for information about the signals at the point of the current
-- frame. In particular, you can call get_width() and get_height()
-- for any signal number, and use that to e.g. assist in chain selection.
--
-- You should return two objects; the chain itself, and then a
-- function (taking no parameters) that is run just before rendering.
-- The function needs to call connect_signal on any inputs, so that
-- it gets updated video data for the given frame. (You are allowed
-- to switch which input your input is getting from between frames,
-- but not calling connect_signal results in undefined behavior.)
-- If you want to change any parameters in the chain, this is also
-- the right place.
--
-- NOTE: The chain returned must be finalized with the Y'CbCr flag
-- if and only if num==0.
function get_chain(num, t, width, height, signals)
	local chain, signal_num
	if num == 0 then  -- Live (right pane).
		chain = simple_hq_chain
		signal_num = live_signal_num
	elseif num == 1 then  -- Preview (left pane).
		chain = simple_lq_chain
		signal_num = preview_signal_num
	else  -- One of the two previews (bottom panes).
		chain = simple_lq_chain
		signal_num = num - 2
	end

	prepare = function()
		chain.input:connect_signal(signal_num)
		local color = input_neutral_color[signal_num + 1]
		chain.wb_effect:set_vec3("neutral_color", color[1], color[2], color[3])
	end
	return chain.chain, prepare
end
