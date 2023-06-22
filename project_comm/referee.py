import logging
import asyncio
import aiocoap
from enum import Enum
import os
from pydub import AudioSegment
from pydub.playback import play
import re

WINNING_PUSHUP_COUNT = 4


class PlayerColor(Enum):
    OFF = 0
    RED = 1
    GREEN = 2
    BLUE = 3


class Player:
    host: str
    color: PlayerColor

    def __init__(self, host: str, color: PlayerColor):
        self.host = host
        self.color = color


# logging setup
logging.basicConfig(level=logging.INFO)
logging.getLogger("coap-server").setLevel(logging.DEBUG)


async def discover_dictionary():
    protocol = await aiocoap.Context.create_client_context()

    message = aiocoap.Message(
        mtype=aiocoap.Type.NON,
        code=aiocoap.Code.GET,
        uri="coap://[ff02::1]/.well-known/core?rt=core.rd*",
    )

    try:
        response = await protocol.request(message).response
    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
        return None
    else:
        return str(response.remote.hostinfo)


async def discover_players(rd_address: str):
    protocol = await aiocoap.Context.create_client_context()

    message = aiocoap.Message(
        code=aiocoap.Code.GET,
        uri=f"coap://{rd_address}/endpoint-lookup/?rt=pushups_player",
    )

    try:
        response = await protocol.request(message).response

    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
    else:
        payload = response.payload.decode("utf-8")
        lines = payload.split(",")
        endpoints = {
            re.search(r'base="(.*?)"', line).group(1).replace("coap://", "")
            for line in lines
        }
        print("\n\nASDF\n\n")
        print(endpoints)
        print("\n\nASDF\n\n")
        player = {
            Player(endpoint, PlayerColor(index + 1))
            for index, endpoint in enumerate(endpoints)
        }
        return player


async def start_game(players: set[Player]):
    protocol = await aiocoap.Context.create_client_context()

    # assign id to each player and start the game
    for player in players:
        message = aiocoap.Message(
            code=aiocoap.Code.PUT,
            uri=f"coap://{player.host}/assign_color_id",
            payload=f"{player.color.value}".encode("ascii"),
        )

        print("PLAYER COLOR: ", player.color)

        await protocol.request(message).response

    # observe the count of each player
    def observation_callback(response):
        pushup_count = int(response.payload)
        player = list(
            filter(
                lambda player: player.host == str(response.remote.hostinfo),
                players,
            )
        )[0]
        if pushup_count == WINNING_PUSHUP_COUNT:
            print(f"The winner is: {player.color._name_} {player.host}")
            play_winner_sound(player.color)
            exit()
        else:
            print(f"{player.color._name_} pushup count: {pushup_count}")
            play_counter_sound(player.color)

    async def observe_resource(uri: str):
        message = aiocoap.Message(code=aiocoap.Code.GET)
        message.set_request_uri(uri)
        # set observe bit from None to 0
        message.opt.observe = 0
        observation_is_over = asyncio.get_event_loop().create_future()

        request = protocol.request(message)
        try:
            if request.observation:
                request.observation.register_callback(observation_callback)

                await request.response
                await observation_is_over
        finally:
            if not request.response.done():
                request.response.cancel()
            if request.observation and not request.observation.cancelled:
                request.observation.cancel()

    tasks = [observe_resource(f"coap://{player.host}/count") for player in players]
    await asyncio.gather(*tasks)


def play_counter_sound(player_color: PlayerColor) -> None:
    if player_color == PlayerColor.GREEN:
        play(
            AudioSegment.from_mp3(
                f"{os.path.dirname(os.path.abspath(__file__))}/audio/gruen.mp3"
            )
        )
    elif player_color == PlayerColor.RED:
        play(
            AudioSegment.from_mp3(
                f"{os.path.dirname(os.path.abspath(__file__))}/audio/rot.mp3"
            )
        )


def play_winner_sound(player_color: PlayerColor) -> None:
    if player_color == PlayerColor.GREEN:
        play(
            AudioSegment.from_mp3(
                f"{os.path.dirname(os.path.abspath(__file__))}/audio/gruen_hat_gewonnen.mp3"
            )
        )
    elif player_color == PlayerColor.RED:
        play(
            AudioSegment.from_mp3(
                f"{os.path.dirname(os.path.abspath(__file__))}/audio/rot_hat_gewonnen.mp3"
            )
        )


async def main():
    # Discover resource dictionary via multicast
    resource_directory_ip_address = await discover_dictionary()

    if resource_directory_ip_address:
        # Discover players in resource directory
        players = await discover_players(resource_directory_ip_address)

        if players:
            # Print available player addresses
            print("Found players:")
            for player in players:
                print(f"{player.host}")

            # Start Game
            await start_game(players)


if __name__ == "__main__":
    play_winner_sound(PlayerColor.RED)
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
