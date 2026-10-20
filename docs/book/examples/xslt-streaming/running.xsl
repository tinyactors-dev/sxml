<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>

  <xsl:template match="/orders">
    <!-- Go through the orders once, keeping a running total, and stop
         at the first order that takes it past 1,000. -->
    <xsl:iterate select="order">
      <xsl:param name="sum" as="xs:decimal" select="0"/>
      <xsl:variable name="next" select="$sum + xs:decimal(total)"/>
      <xsl:choose>
        <xsl:when test="$next gt 1000">
          <xsl:break>Order {@id} passes 1,000: the total is {$next}.</xsl:break>
        </xsl:when>
        <xsl:otherwise>
          <xsl:next-iteration>
            <xsl:with-param name="sum" select="$next"/>
          </xsl:next-iteration>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:iterate>
  </xsl:template>
</xsl:stylesheet>
