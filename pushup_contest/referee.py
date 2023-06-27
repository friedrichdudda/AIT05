import logging
import asyncio
import aiocoap
from enum import Enum
import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor


WINNING_PUSHUP_COUNT = 10


class PlayerColor(Enum):
    OFF = 0
    RED = 1
    GREEN = 2
    BLUE = 3


class Player:
    host: str
    color: PlayerColor
    count: int = 0

    def __init__(self, host: str, color: PlayerColor):
        self.host = host
        self.color = color


# logging setup
logging.basicConfig(level=logging.ERROR)
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
        if payload == "":
            print("No entries in resource directory")
            exit()
        lines = payload.split(",")
        endpoints = set()
        for line in lines:
            match = re.search(r'base="(.*?)"', line)
            if match is not None:
                endpoint = match.group(1).replace("coap://", "")
                endpoints.add(endpoint)
            else:
                print("Incorrect entry in resource directory")

        player = {
            Player(endpoint, PlayerColor(index + 1))
            for index, endpoint in enumerate(endpoints)
        }
        return player


async def assign_player_colors(players: set[Player]):
    protocol = await aiocoap.Context.create_client_context()

    for player in players:
        message = aiocoap.Message(
            code=aiocoap.Code.PUT,
            uri=f"coap://{player.host}/assign_color",
            payload=f"{player.color.value}".encode("ascii"),
        )

        await protocol.request(message).response


async def observe_players(players: set[Player]):
    protocol = await aiocoap.Context.create_client_context()

    # observe the count of each player
    def observation_callback(response):
        pushup_count = int(response.payload)
        player = list(
            filter(
                lambda player: player.host == str(response.remote.hostinfo),
                players,
            )
        )[0]

        player.count = int(response.payload.decode("utf-8"))

        if pushup_count == WINNING_PUSHUP_COUNT:
            print(f"The winner is: {player.color.name} {player.host}")

            # set player to winning state
            message = aiocoap.Message(
                code=aiocoap.Code.POST,
                uri=f"coap://{player.host}/set_to_winner",
            )
            protocol.request(message).response

            # set other players to loosing state
            for loosing_player in players:
                if loosing_player != player:
                    message = aiocoap.Message(
                        code=aiocoap.Code.POST,
                        uri=f"coap://{loosing_player.host}/set_to_looser",
                    )
                    protocol.request(message).response

            # play winning player sound
            play_winner_sound(player.color)
        else:
            print(f"{player.color.name} pushup count: {pushup_count}")
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


async def start_game_cli(players: set[Player]):
    protocol = await aiocoap.Context.create_client_context()

    async def ainput(prompt: str = ""):
        with ThreadPoolExecutor(1, "ainput") as executor:
            return (
                await asyncio.get_event_loop().run_in_executor(executor, input, prompt)
            ).rstrip()

    while True:
        print("")
        command = await ainput("")

        if command == "help":
            print("Available commands: help | list | stats | start | reset")

        elif command == "list":
            print("Available players:")
            for player in players:
                print(f"{player.color.name} ({player.host})")

        elif command == "start":
            for player in players:
                message = aiocoap.Message(
                    code=aiocoap.Code.POST,
                    uri=f"coap://{player.host}/start",
                )

                await protocol.request(message).response

            print("Game started:")

        elif command == "stats":
            for player in players:
                print(f"{player.color.name}: {player.count}")

        elif command == "reset":
            # reset count of all players
            for player in players:
                player.count = 0
                message = aiocoap.Message(
                    code=aiocoap.Code.POST,
                    uri=f"coap://{player.host}/reset",
                )

                await protocol.request(message).response

            print("Players have been reset.")


def play_counter_sound(player_color: PlayerColor) -> None:
    if player_color == PlayerColor.GREEN:
        play_audio_file(f"{os.path.dirname(os.path.abspath(__file__))}/audio/gruen.mp3")
    elif player_color == PlayerColor.RED:
        play_audio_file(f"{os.path.dirname(os.path.abspath(__file__))}/audio/rot.mp3")


def play_winner_sound(player_color: PlayerColor) -> None:
    if player_color == PlayerColor.GREEN:
        play_audio_file(
            f"{os.path.dirname(os.path.abspath(__file__))}/audio/gruen_hat_gewonnen.mp3"
        )
    elif player_color == PlayerColor.RED:
        play_audio_file(
            f"{os.path.dirname(os.path.abspath(__file__))}/audio/rot_hat_gewonnen.mp3"
        )


def play_audio_file(file_url: str):
    """subprocess.run(
        [
            "ffplay",
            "-v",
            "0",
            "-nodisp",
            "-autoexit",
            file_url,
        ],
    )"""


async def main():
    # Discover resource dictionary via multicast
    resource_directory_ip_address = await discover_dictionary()

    if resource_directory_ip_address:
        # Discover players in resource directory
        players = await discover_players(resource_directory_ip_address)

        if players:
            await assign_player_colors(players)

            # Start Game
            await asyncio.gather(
                observe_players(players),
                start_game_cli(players),
            )


if __name__ == "__main__":
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
