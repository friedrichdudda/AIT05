import logging
import asyncio
import aiocoap.resource as resource
import aiocoap


class Player:
    host: str = ""
    count: int = 0

    def __init__(self, host: str):
        self.host = host


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
        player = {Player(endpoint) for endpoint in endpoints}
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
    """ messages = [
        (
            aiocoap.Message(
                code=aiocoap.Code.GET, uri=f"coap://{player.host}/count", observe=0
            )
        )
        for player in players
    ] """

    def observation_callback(response):
        print("callback: %r" % response.payload)

    message = aiocoap.Message(code=aiocoap.Code.GET)
    message.set_request_uri(f"coap://{list(players)[0].host}/count")
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
