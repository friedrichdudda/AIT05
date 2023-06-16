import logging
import asyncio
import aiocoap
from enum import Enum

WINNING_PUSHUP_COUNT = 4


class PlayerColor(Enum):
    YELLOW = 0
    ORANGE = 1
    BLUE = 2
    PINK = 3
    PURPLE = 4


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
        uri=f"coap://{rd_address}/resource-lookup/?rt=pushups_player",
    )

    try:
        response = await protocol.request(message).response

    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
    else:
        payload = response.payload.decode("utf-8")
        lines = payload.split(",")
        endpoints = {str(line.split(";")[0].split("/")[2]) for line in lines}
        player = {
            Player(endpoint, PlayerColor(index)) for index, endpoint in enumerate(endpoints)
        }
        return player


async def start_game(players: set[Player]):
    protocol = await aiocoap.Context.create_client_context()

    # assign id to each player and start the game
    for index, player in enumerate(players):
        message = aiocoap.Message(
            code=aiocoap.Code.PUT,
            uri=f"coap://{player.host}/assign_player_id",
            payload=f"{index}".encode("ascii"),
        )

        await protocol.request(message).response

    # observe the count of each player
    def observation_callback(response):
        print("callback: %r" % response.payload)
        pushup_count = int(response.payload)
        if pushup_count == WINNING_PUSHUP_COUNT:
            winner = list(
                filter(
                    lambda player: player.host == str(response.remote.hostinfo),
                    players,
                )
            )[0]

            print(f"The winner is: {winner.color._name_} {winner.host}")

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
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())
