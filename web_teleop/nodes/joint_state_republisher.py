#!/usr/bin/env python

import rospy
from std_msgs.msg import Float64
from joint_state_reader import JointStateReader


def wait_for_time():
    """Wait for simulated time to begin.
    """
    while rospy.Time().now().to_sec() == 0:
        pass


def main():
    rospy.init_node('joint_state_republisher')
    wait_for_time()

    torso_pub = rospy.Publisher('joint_state_republisher/torso_lift_joint',
                                Float64, queue_size=10)
    head_pan_pub = rospy.Publisher('joint_state_republisher/head_pan_joint',
                                Float64, queue_size=10)
    head_tilt_pub = rospy.Publisher('joint_state_republisher/head_tilt_joint',
                                Float64, queue_size=10)

    reader = JointStateReader()
    rospy.sleep(0.5)

    rate = rospy.Rate(10)
    while not rospy.is_shutdown():
        # TODO: get joint value
        torso_joint_value = reader.get_joint('torso_lift_joint')
        head_pan_value = reader.get_joint('head_pan_joint')
        head_tilt_value = reader.get_joint('head_tilt_joint')

        # TODO: publish joint value
        torso_pub.publish(torso_joint_value)
        head_pan_pub.publish(head_pan_value)
        head_tilt_pub.publish(head_tilt_value)

        rate.sleep()


if __name__ == '__main__':
    main()
