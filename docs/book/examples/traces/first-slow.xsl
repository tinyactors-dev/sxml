<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes"/>
  <xsl:output method="text"/>
  <!-- A parameter from the command line is untyped: convert it. -->
  <xsl:param name="threshold" select="80"/>

  <xsl:template match="/traces">
    <!-- Go through the traces once, counting them, and stop at the
         first request that took longer than the threshold. -->
    <xsl:iterate select="trace">
      <xsl:param name="faster" as="xs:integer" select="0"/>
      <xsl:on-completion>No request took longer than {$threshold} ms.&#10;</xsl:on-completion>
      <xsl:variable name="took" select="xs:decimal(span/@duration)"/>
      <xsl:choose>
        <xsl:when test="$took gt xs:decimal($threshold)">
          <xsl:break>{@route} at {@start} took {$took} ms, after {$faster} faster requests.&#10;</xsl:break>
        </xsl:when>
        <xsl:otherwise>
          <xsl:next-iteration>
            <xsl:with-param name="faster" select="$faster + 1"/>
          </xsl:next-iteration>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:iterate>
  </xsl:template>
</xsl:stylesheet>
